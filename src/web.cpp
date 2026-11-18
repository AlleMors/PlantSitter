#include "config.h"
#include "web.h"
#include "store.h"
#include "scheduler.h"
#include "rtc.h"
#include "logger.h"

#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <vector>
#include <functional>
#include <Updater.h>

namespace {
    AsyncWebServer server(WEB_PORT);

    // TTL in epoch (non millis): resiste all'overflow a ~49 giorni.
    struct Session { String id; time_t expiresAt; };
    Session sessions[SESSION_MAX];

    const char* COOKIE_ATTR = "; Path=/; HttpOnly; SameSite=Strict";

    // -----------------------------------------------------------------
    // Sessioni
    // -----------------------------------------------------------------
    String makeSid() {
        String s;
        for (int i = 0; i < 4; ++i) {
            char b[9];
            snprintf(b, sizeof(b), "%08x", (uint32_t)random());
            s += b;
        }
        return s;
    }

    String cookieValue(AsyncWebServerRequest* req, const char* name) {
        if (!req->hasHeader("Cookie")) return "";
        const AsyncWebHeader* h = req->getHeader("Cookie");
        if (!h) return "";
        String c = h->value();
        String key = String(name) + "=";
        int p = c.indexOf(key);
        if (p < 0) return "";
        p += key.length();
        int end = c.indexOf(';', p);
        return end < 0 ? c.substring(p) : c.substring(p, end);
    }

    Session* findSession(const String& sid) {
        time_t now = time(nullptr);
        for (auto& s : sessions) {
            if (!s.id.isEmpty() && s.id == sid && s.expiresAt > now) return &s;
        }
        return nullptr;
    }

    bool authorized(AsyncWebServerRequest* req) {
        return findSession(cookieValue(req, SESSION_COOKIE)) != nullptr;
    }

    void createSession(AsyncWebServerRequest* req) {
        time_t now = time(nullptr);
        int slot = -1;
        for (int i = 0; i < SESSION_MAX; ++i) {
            if (sessions[i].id.isEmpty()) { slot = i; break; }
        }
        if (slot < 0) { // eviction del più vecchio
            slot = 0;
            for (int i = 1; i < SESSION_MAX; ++i)
                if (sessions[i].expiresAt < sessions[slot].expiresAt) slot = i;
        }
        sessions[slot].id = makeSid();
        sessions[slot].expiresAt = now + (time_t)SESSION_TTL_SEC;

        AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", "{\"ok\":true}");
        resp->addHeader("Set-Cookie", String(SESSION_COOKIE) + "=" + sessions[slot].id + COOKIE_ATTR);
        req->send(resp);
    }

    void destroySession(AsyncWebServerRequest* req) {
        String sid = cookieValue(req, SESSION_COOKIE);
        for (auto& s : sessions) if (s.id == sid) s.id = "";
        AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", "{\"ok\":true}");
        resp->addHeader("Set-Cookie", String(SESSION_COOKIE) + "=" + COOKIE_ATTR + "; Max-Age=0");
        req->send(resp);
    }

    // -----------------------------------------------------------------
    // Risposte
    // -----------------------------------------------------------------
    void sendJson(AsyncWebServerRequest* req, int code, const String& body) {
        AsyncWebServerResponse* resp = req->beginResponse(code, "application/json", body);
        resp->addHeader("Cache-Control", "no-store");
        req->send(resp);
    }

    void sendErr(AsyncWebServerRequest* req, int code, const String& msg) {
        JsonDocument d;
        d["error"] = msg;
        String s;
        serializeJson(d, s);
        sendJson(req, code, s);
    }

    bool requireAuth(AsyncWebServerRequest* req) {
        if (authorized(req)) return true;
        sendErr(req, 401, "sessione non valida o scaduta");
        return false;
    }

    // -----------------------------------------------------------------
    // Payload JSON
    // -----------------------------------------------------------------
    String zoneStateJson() {
        JsonDocument doc;
        JsonArray arr = doc["zones"].to<JsonArray>();
        for (uint8_t z = 1; z <= MAX_ZONES; ++z) {
            const ZoneState& st = sched::state(z);
            JsonObject o = arr.add<JsonObject>();
            o["id"] = z;
            o["open"] = st.open;
            o["remaining"] = st.remainingSec;
            o["used_today"] = st.todayUsedSec;
            o["reason"] = st.reason == RUN_MANUAL ? "manual" : "schedule";
        }
        doc["active"] = sched::activeZone();
        doc["clock_valid"] = psrtc::clockValid();
        doc["rtc_present"] = psrtc::rtcPresent();
        doc["rtc_valid"] = psrtc::rtcValid();

        time_t now = time(nullptr);
        doc["now"] = (long)now;
        struct tm tm;
        localtime_r(&now, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
        doc["iso"] = buf;

        String out;
        serializeJson(doc, out);
        return out;
    }

    String statusJson() {
        JsonDocument doc;
        doc["version"] = PS_VERSION;
        doc["device_name"] = store::deviceName();
        doc["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
        doc["mac"] = WiFi.macAddress();
        doc["rssi"] = WiFi.RSSI();
        doc["uptime_s"] = millis() / 1000UL;
        doc["heap_free"] = ESP.getFreeHeap();
        doc["has_password"] = store::hasPassword();
        doc["tz"] = store::tzRule();
        doc["active"] = sched::activeZone();
        String out;
        serializeJson(doc, out);
        return out;
    }

    // -----------------------------------------------------------------
    // Handler body (POST con JSON)
    // -----------------------------------------------------------------
    // Usati come onBodyHandler: su ESPAsyncWebServer il body di un POST
    // arriva in uno o più chunk, e *total* è la lunghezza totale dichiarata.
    // Per i payload piccoli arriva in un colpo; per /api/config che può
    // essere più grande, accumuliamo in un buffer per-request.
    struct BodyAcc {
        AsyncWebServerRequest* req;
        String body;
        bool rejected;   // limite superato: smettiamo di accumulare
        bool chunked;    // Transfer-Encoding: chunked (total non affidabile)
    };
    std::vector<BodyAcc*> bodies;

    void releaseBody(AsyncWebServerRequest* req) {
        for (size_t i = 0; i < bodies.size(); ++i) {
            if (bodies[i]->req == req) { delete bodies[i]; bodies.erase(bodies.begin() + i); return; }
        }
    }

    BodyAcc* accumulatorFor(AsyncWebServerRequest* req) {
        for (auto* b : bodies) if (b->req == req) return b;
        auto* b = new (std::nothrow) BodyAcc{req, String(), false, false};
        if (!b) return nullptr;
        bodies.push_back(b);
        // Se il client si disconnette a metà, il request viene distrutto:
        // senza questo il buffer resterebbe orfano per sempre.
        req->onDisconnect([req]() { releaseBody(req); });
        return b;
    }

    // Accumula un chunk applicando il limite DIMENSIONE MAN MANO (total può
    // essere 0 con Transfer-Encoding: chunked, quindi non è affidabile).
    // Ritorna true se il body è completo e va processato.
    bool accumulate(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                    size_t index, size_t total, String& out, bool& tooLarge) {
        BodyAcc* acc = accumulatorFor(req);
        if (!acc) { tooLarge = true; return false; }
        if (index == 0) { acc->body = ""; acc->rejected = false; acc->chunked = false; }

        // Rifiutato: NON rilasciamo l'accumulatore (perderemmo lo stato e i
        // chunk successivi verrebbero processati lo stesso), semplicemente
        // ignoriamo il resto del body. Viene liberato alla disconnessione.
        if (acc->rejected) return false;

        // Con chunked il server passa _contentLength (che può essere > 0 se
        // il client ha mandato ANCHE Content-Length): non è affidabile.
        if (total == 0) acc->chunked = true;

        if (acc->body.length() + len > API_BODY_LIMIT) {
            acc->rejected = true;
            acc->body = "";
            tooLarge = true;
        } else {
            acc->body.concat((const char*)data, len);
        }

        // Completamento: con Content-Length usiamo index+len==total; con
        // chunked il server segnala la fine con una chiamata di lunghezza 0.
        bool complete = acc->chunked ? (len == 0) : (total > 0 && index + len >= total);
        if (acc->rejected) {
            if (complete) releaseBody(req);   // 413 già inviato: solo cleanup
            return false;
        }
        if (!complete) return false;
        out = acc->body;
        releaseBody(req);
        return true;
    }

    // -----------------------------------------------------------------
    // Rotte
    // -----------------------------------------------------------------
    // ---- stato OTA ----
    // otaRan/otaOk esistono perché Updater::isFinished() è TRUE anche quando
    // non è avvenuto alcun upload (0 == 0 + 0): senza stato esplicito un
    // POST vuoto riavvierebbe il dispositivo dichiarando successo.
    bool otaRan = false;
    bool otaOk = false;
    unsigned long restartAtMs = 0;

    // Serializzazione dell'upload. Update e otaRan/otaOk sono globali:
    // due upload concorrenti (retry del browser mentre la prima connessione
    // è appesa) mescolerebbero i due file in un'unica immagine.
    AsyncWebServerRequest* otaOwner = nullptr;
    size_t otaBytes = 0;

    // Un .bin ESP8266 valido inizia con 0xE9, ha 1..16 segmenti e un
    // flash mode 0..3. Updater verifica SOLO il primo byte: senza questi
    // controlli anche un file di 4 byte verrebbe committato e armato,
    // lasciando il dispositivo da riflashare via seriale.
    bool otaHeaderSane(const uint8_t* d, size_t n) {
        if (n < 4) return false;
        if (d[0] != 0xE9) return false;
        if (d[1] < 1 || d[1] > 16) return false;
        if (d[2] > 3) return false;
        return true;
    }
    const size_t OTA_MIN_SIZE = 64UL * 1024UL;   // firmware compilato: mai sotto

    // Spazio realmente indirizzabile per l'immagine. Su ESP8266 le
    // partizioni non sono una tabella (partitions.csv NON è letta dal core):
    // il layout è nel linker script eagle.flash.4m1m.ld, con lo sketch in
    // una regione da ~1019 KB (max 1044464 B). Non dobbiamo committare
    // immagini più grandi: _verifyEnd controlla solo il magic byte 0xE9.
    size_t otaMaxSize() {
        const size_t SKETCH_REGION = 1044464UL;
        size_t free = ESP.getFreeSketchSpace();
        size_t cap = free > 0 ? free : 0;
        return cap < SKETCH_REGION ? cap : SKETCH_REGION;
    }

    void registerRoutes() {
        // Middleware: le rotte JSON accettano solo application/json e corpi
        // entro il limite. Senza questo, un x-www-form-urlencoded viene
        // bufferizzato dalla libreria SENZA limite e senza passare dalle
        // callback che controllano la dimensione (~40 KB di heap).
        server.addMiddleware([](AsyncWebServerRequest* req, ArMiddlewareNext next) {
            // /update è esente: multipart grande, handler con skip dei
            // middleware e autenticazione propria nelle callback di upload.
            if (req->url() == "/update") { next(); return; }
            if (req->method() == AsyncWebRequestMethod::HTTP_POST) {
                if (req->contentLength() > API_BODY_LIMIT) {
                    sendErr(req, 413, "body troppo grande");
                    return;
                }
                if (req->hasHeader("Content-Type")) {
                    String ct = req->getHeader("Content-Type")->value();
                    bool json = ct.startsWith("application/json");
                    bool form = ct.startsWith("application/x-www-form-urlencoded");
                    bool multipart = ct.startsWith("multipart/form-data");
                    if ((form || multipart) && req->url().startsWith("/api/")) {
                        sendErr(req, 415, "content-type non supportato");
                        return;
                    }
                    if (!json && !multipart && req->url().startsWith("/api/")
                        && req->url() != "/api/zone/close"
                        && req->url() != "/api/zone/close_all"
                        && req->url() != "/api/logout"
                        && req->url() != "/api/reboot") {
                        sendErr(req, 415, "content-type non supportato");
                        return;
                    }
                }
            }
            next();
        });

        // Serve SOLO gli asset della UI, percorso per percorso. Un
        // serveStatic("/") servirebbe anche /data/config.json (salt+hash
        // della password) e /data/log.txt: LittleFS non ha permessi.
        server.on("/", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest* req) {
            req->send(LittleFS, "/index.html", "text/html");
        });
        server.on("/index.html", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest* req) {
            req->send(LittleFS, "/index.html", "text/html");
        });
        server.on("/app.js", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest* req) {
            req->send(LittleFS, "/app.js", "application/javascript");
        });
        server.on("/style.css", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest* req) {
            req->send(LittleFS, "/style.css", "text/css");
        });

        // ---- OTA ----
        // Handler nativo al posto di ElegantOTA: quello scorre/scrive la
        // flash DURANTE il parsing del multipart, prima che un middleware
        // possa rispondere 401. Qui la sessione è verificata nella stessa
        // callback di upload, prima di qualunque Update.begin()/write().
        server.on("/update", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest* req) {
            if (!requireAuth(req)) return;
            char page[420];
            snprintf_P(page, sizeof(page), PSTR(
                "<!doctype html><meta charset=utf-8><title>OTA</title>"
                "<h2>Aggiornamento firmware</h2><p>Immagine .bin, massimo %u KB.</p>"
                "<form method=post action=/update enctype=multipart/form-data>"
                "<input type=file name=firmware accept=.bin required><button>Carica</button>"
                "</form><p><a href=/>Torna alla UI</a></p>"), (unsigned)(otaMaxSize() / 1024));
            req->send(200, "text/html", page);
        });

        // /update: esente dai middleware JSON (multipart, grandi, e senza
        // Content-Type JSON). La protezione è nei callback qui sotto.
        server.on("/update", AsyncWebRequestMethod::HTTP_POST,
            [](AsyncWebServerRequest* req) {
                // I flag appartengono all'upload in corso: se questa richiesta
                // non ne era il proprietario (es. un secondo client), non
                // consumiamo l'esito di qualcun altro.
                if (otaOwner != req) {
                    if (!requireAuth(req)) return;
                    // Un secondo client ha tentato un upload mentre un altro
                    // era in corso, oppure la richiesta non ha file part.
                    sendErr(req, 409, "upload OTA non associato o già in corso");
                    return;
                }
                otaOwner = nullptr;
                if (!requireAuth(req)) {      // 401 se la sessione è scaduta
                    otaRan = false; otaOk = false;
                    return;
                }
                bool ok = otaOk && !Update.hasError();
                // Il flag è per-richiesta: senza questo reset, un POST vuoto
                // dopo un upload riuscito riporterebbe "completato" (e
                // riavvierebbe) basandosi sull'esito del precedente.
                bool ran = otaRan;
                otaRan = false;
                otaOk = false;
                if (!ran) {
                    sendErr(req, 400, "nessun file ricevuto");
                    return;
                }
                if (ok) {
                    req->send(200, "text/plain", "Aggiornamento completato, riavvio...");
                    // Riavvio DIFFERITO: ESP.restart() dentro il handler
                    // interromperebbe la catena e la risposta non partirebbe.
                    restartAtMs = millis() + 1200;
                } else {
                    char msg[128];
                    snprintf_P(msg, sizeof(msg), PSTR("Aggiornamento fallito: %s"),
                               Update.getErrorString().c_str());
                    req->send(500, "text/plain", msg);
                }
            },
            [](AsyncWebServerRequest* req, const String&, size_t index,
               uint8_t* data, size_t len, bool final) {
                // N.B. questo è l'handler di UPLOAD (non onBody): deve
                // esserci un solo overload con la firma a 6 parametri.
                // 1) Autorizzazione PRIMA di toccare la flash.
                if (!requireAuth(req)) return;

                // 2) Un solo upload alla volta. Update e i flag sono globali:
                //    due upload concorrenti mescolerebbero i due file.
                if (index == 0) {
                    if (otaOwner && otaOwner != req) {
                        return;                 // 409 già risposto dal handler
                    }
                    // Upload precedente abbandonato: azzera senza committare.
                    if (Update.isRunning()) {
                        Update.end(false);
                        PS_LOG(LOG_WARN, "OTA: annullato upload precedente incompleto");
                    }
                    otaOwner = req;
                    otaBytes = 0;
                    otaOk = false;
                    otaRan = false;
                    sched::closeAll();          // sicurezza: nessuna valvola aperta
                    if (!Update.begin(otaMaxSize())) {
                        PS_LOG(LOG_ERROR, "OTA: begin rifiutato: %s",
                               Update.getErrorString().c_str());
                        otaOwner = nullptr;
                        return;
                    }
                }
                // 3) Solo il proprietario scrive, e solo se begin è riuscito.
                if (otaOwner != req || !Update.isRunning()) return;

                if (len > 0) {
                    // Validazione dell'header al primo blocco.
                    if (otaBytes == 0 && !otaHeaderSane(data, len)) {
                        PS_LOG(LOG_ERROR, "OTA: header non valido, upload annullato");
                        Update.end(false);
                        otaOwner = nullptr;
                        return;
                    }
                    if (!Update.hasError() && Update.write(data, len) != len) {
                        // Serial non è disponibile in questo pinout.
                        PS_LOG(LOG_ERROR, "OTA: scrittura fallita al byte %u",
                               (unsigned)otaBytes);
                    }
                    otaBytes += len;
                }
                if (final) {
                    // Rifiuta immagini troppo piccole o incomplete: Updater
                    // controlla solo il magic byte, e un'immagine troncata
                    // armata nel bootloader rende il dispositivo inavviabile.
                    bool plausible = otaBytes >= OTA_MIN_SIZE && otaBytes <= otaMaxSize();
                    if (!Update.hasError() && plausible && Update.end(true)) {
                        otaOk = true;
                        PS_LOG(LOG_INFO, "OTA: immagine pronta (%u byte)",
                               (unsigned)otaBytes);
                    } else {
                        if (!plausible) {
                            PS_LOG(LOG_ERROR, "OTA: immagine rifiutata (%u byte)",
                                   (unsigned)otaBytes);
                        }
                        Update.end(false);      // non committare l'incompleto
                    }
                    otaOwner = nullptr;
                }
            }).setSkipServerMiddlewares(true);

        // AUTH
        server.on("/api/login", AsyncWebRequestMethod::HTTP_POST,
            [](AsyncWebServerRequest*) { /* body gestito in onBody */ },
            [](AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool) {},
            [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
                String body;
                bool tooLarge = false;
                if (!accumulate(req, data, len, index, total, body, tooLarge)) {
                    if (tooLarge) sendErr(req, 413, "body troppo grande");
                    return;
                }

                JsonDocument d;
                if (deserializeJson(d, body)) { sendErr(req, 400, "JSON non valido"); return; }
                String user = d["user"] | "";
                String pass = d["pass"] | "";
                if (!store::hasPassword()) { sendErr(req, 409, "nessuna password configurata"); return; }
                if (!store::checkPassword(user, pass)) {
                    PS_LOG(LOG_WARN, "login fallito per user '%s'", user.c_str());
                    // backoff progressivo: 0.5 s, 1 s, 2 s, 4 s, max 8 s
                    static uint8_t fails = 0;
                    uint32_t backoff = fails < 5 ? (500UL << fails) : 8000UL;
                    if (fails < 255) fails++;
                    delay(backoff);
                    sendErr(req, 403, "credenziali errate");
                    return;
                }
                createSession(req);
            });

        server.on("/api/logout", AsyncWebRequestMethod::HTTP_POST, [](AsyncWebServerRequest* req) {
            destroySession(req);
        });

        server.on("/api/session", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest* req) {
            JsonDocument d;
            d["authenticated"] = authorized(req);
            d["has_password"] = store::hasPassword();
            String s;
            serializeJson(d, s);
            sendJson(req, 200, s);
        });

        server.on("/api/setup", AsyncWebRequestMethod::HTTP_POST,
            [](AsyncWebServerRequest*) {},
            [](AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool) {},
            [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
                String body;
                bool tooLarge = false;
                if (!accumulate(req, data, len, index, total, body, tooLarge)) {
                    if (tooLarge) sendErr(req, 413, "body troppo grande");
                    return;
                }

                if (store::hasPassword()) { sendErr(req, 409, "già configurato"); return; }
                JsonDocument d;
                if (deserializeJson(d, body)) { sendErr(req, 400, "JSON non valido"); return; }
                String user = d["user"] | "";
                String pass = d["pass"] | "";
                if (!store::setPassword(user, pass)) {
                    sendErr(req, 400, "utente vuoto o password < 6 caratteri");
                    return;
                }
                // Se la scrittura fallisce la password esisterebbe solo in RAM:
                // al riavvio successivo il dispositivo sarebbe di nuovo aperto.
                if (!store::save()) {
                    store::clearPassword();
                    sendErr(req, 500, "scrittura configurazione fallita");
                    return;
                }
                createSession(req);
            });

        // CONFIG
        server.on("/api/config", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest* req) {
            if (!requireAuth(req)) return;
            sendJson(req, 200, store::toJson());
        });
        server.on("/api/config", AsyncWebRequestMethod::HTTP_POST,
            [](AsyncWebServerRequest* req) {
                if (!requireAuth(req)) return;
            },
            [](AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool) {},
            [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
                if (!requireAuth(req)) return;
                String body;
                bool tooLarge = false;
                if (!accumulate(req, data, len, index, total, body, tooLarge)) {
                    if (tooLarge) sendErr(req, 413, "body troppo grande");
                    return;
                }

                String snapshot = store::toJson();
                String err;
                if (!store::fromJson(body, err)) { sendErr(req, 400, err); return; }
                if (!store::save()) {
                    // Il file su flash può essere rimasto intatto (vedi
                    // writeAtomic): riportiamo la RAM allo stato precedente,
                    // senza affermare che la config su disco è stata ripristinata.
                    String e2; store::fromJson(snapshot, e2);
                    sendErr(req, 500, "scrittura fallita; configurazione non modificata");
                    return;
                }
                // Il fuso orario va applicato subito, non al prossimo riavvio.
                psrtc::applyTimezone(store::tzRule());
                sendJson(req, 200, "{\"ok\":true}");
            });

        // ZONES runtime
        server.on("/api/zones", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest* req) {
            if (!requireAuth(req)) return;
            sendJson(req, 200, zoneStateJson());
        });

        server.on("/api/zone/open", AsyncWebRequestMethod::HTTP_POST,
            [](AsyncWebServerRequest*) {},
            [](AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool) {},
            [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
                // Auth PRIMA di allocare/accumulare: niente buffer per ignoti.
                if (!requireAuth(req)) return;
                String body;
                bool tooLarge = false;
                if (!accumulate(req, data, len, index, total, body, tooLarge)) {
                    if (tooLarge) sendErr(req, 413, "body troppo grande");
                    return;
                }

                JsonDocument d;
                if (deserializeJson(d, body)) { sendErr(req, 400, "JSON non valido"); return; }
                uint8_t zone = d["zone"] | 0;
                uint16_t seconds = d["seconds"] | 0;
                String err;
                if (!sched::openZone(zone, seconds, RUN_MANUAL, err)) { sendErr(req, 409, err); return; }
                sendJson(req, 200, zoneStateJson());
            });

        server.on("/api/zone/close", AsyncWebRequestMethod::HTTP_POST, [](AsyncWebServerRequest* req) {
            if (!requireAuth(req)) return;
            String r = "manuale";
            if (!sched::closeZone(r)) { sendErr(req, 409, "nessuna zona aperta"); return; }
            sendJson(req, 200, zoneStateJson());
        });

        server.on("/api/zone/close_all", AsyncWebRequestMethod::HTTP_POST, [](AsyncWebServerRequest* req) {
            if (!requireAuth(req)) return;
            sched::closeAll();
            sendJson(req, 200, zoneStateJson());
        });

        // STATUS / DEBUG (dati del dispositivo: richiedono la sessione)
        server.on("/api/status", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest* req) {
            if (!requireAuth(req)) return;
            sendJson(req, 200, statusJson());
        });

        server.on("/api/debug", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest* req) {
            if (!requireAuth(req)) return;
            sendJson(req, 200, "{\"log\":" + pslog::dumpJson(200) + "}");
        });

        server.on("/api/log", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest* req) {
            if (!requireAuth(req)) return;
            req->send(200, "text/plain", pslog::dumpText(100));
        });

        server.on("/api/reboot", AsyncWebRequestMethod::HTTP_POST, [](AsyncWebServerRequest* req) {
            if (!requireAuth(req)) return;
            sched::closeAll();
            sendJson(req, 200, "{\"ok\":true}");
            delay(300);
            ESP.restart();
        });

        // 404 -> JSON per /api/*, index per le rotte SPA, 404 per il resto.
        // Solo GET: un 404 non deve rimandare l'index a un POST.
        server.onNotFound([](AsyncWebServerRequest* req) {
            if (req->url().startsWith("/api/")) { sendErr(req, 404, "endpoint sconosciuto"); return; }
            if (req->method() == AsyncWebRequestMethod::HTTP_GET) {
                req->send(LittleFS, "/index.html", "text/html");
                return;
            }
            sendErr(req, 404, "non trovato");
        });
    }
}

namespace web {
    void begin() {
        registerRoutes();
        server.begin();
        PS_LOG(LOG_INFO, "web server su porta %d", WEB_PORT);
    }

    void loop() {
        // Riavvio post-OTA differito: la risposta HTTP deve uscire prima che
        // ESP.restart() fermi tutto.
        if (restartAtMs && (int32_t)(millis() - restartAtMs) >= 0) {
            restartAtMs = 0;
            ESP.restart();
        }
    }

    void onConnected() {
        PS_LOG(LOG_INFO, "IP %s, mDNS http://%s.local", WiFi.localIP().toString().c_str(), PS_HOSTNAME);
    }
}
