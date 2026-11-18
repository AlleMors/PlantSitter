#include "config.h"
#include "store.h"
#include "logger.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Hash.h>

namespace {
    const char* CONFIG_PATH = DATA_DIR "/config.json";
    const char* CONFIG_BAK  = DATA_DIR "/config.bak";
    const char* CONFIG_TMP  = DATA_DIR "/config.tmp";

    ZoneConfig zones[MAX_ZONES];
    String     tz = TIMEZONE_FALLBACK;
    String     devName = PS_HOSTNAME;
    String     authUser;
    String     authSalt;
    String     authHash;

    String hashPassword(const String& pass, const String& salt) {
        // SHA1(salt + ":" + password): sufficiente per evitare il cleartext
        // su flash e nei backup; non è scrypt/argon2.
        return sha1(salt + ":" + pass);
    }

    String randomHex(size_t bytes) {
        String out;
        for (size_t i = 0; i < bytes; ++i) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", (uint8_t)random(0, 256));
            out += buf;
        }
        return out;
    }

    void defaults() {
        for (int i = 0; i < MAX_ZONES; ++i) {
            zones[i] = ZoneConfig{};
            zones[i].name = String("Zona ") + (i + 1);
        }
    }

    // Scrittura a generazioni: config.tmp -> (rename) config.json, con la
    // generazione precedente conservata in config.bak. Una caduta di tensione
    // in qualsiasi punto lascia sempre almeno un file valido; load() prova
    // json -> bak -> tmp.
    bool writeAtomic(const String& body) {
        if (!LittleFS.exists(DATA_DIR)) LittleFS.mkdir(DATA_DIR);

        File f = LittleFS.open(CONFIG_TMP, "w");
        if (!f) return false;
        size_t n = f.print(body);
        f.close();
        if (n != body.length()) { LittleFS.remove(CONFIG_TMP); return false; }

        // La generazione buona diventa il backup (se esiste)
        if (LittleFS.exists(CONFIG_PATH)) {
            LittleFS.remove(CONFIG_BAK);
            if (!LittleFS.rename(CONFIG_PATH, CONFIG_BAK)) {
                // niente backup: proseguiamo comunque, config.json è intatto
                PS_LOG(LOG_WARN, "backup configurazione non riuscito");
            }
        }
        if (!LittleFS.rename(CONFIG_TMP, CONFIG_PATH)) return false;
        // Il backup NON viene cancellato: serve se config.json si corrompe
        // più tardi (settori, scritture parziali). load() lo usa come
        // fallback e mantiene quindi anche la password.
        return true;
    }
}

namespace store {

bool begin() {
    return LittleFS.begin();
}

bool load() {
    defaults();
    const char* candidates[] = { CONFIG_PATH, CONFIG_BAK, CONFIG_TMP };
    for (const char* path : candidates) {
        File f = LittleFS.open(path, "r");
        if (!f) continue;
        String body = f.readString();
        f.close();
        String err;
        if (fromJson(body, err)) {
            if (strcmp(path, CONFIG_PATH) != 0) {
                PS_LOG(LOG_WARN, "recuperata configurazione da %s", path);
            }
            return true;
        }
        PS_LOG(LOG_ERROR, "%s illeggibile: %s", path, err.c_str());
    }
    PS_LOG(LOG_ERROR, "nessuna configurazione valida: parte con i default "
           "e NESSUNA password impostata");
    return false;
}

bool save() { return writeAtomic(toJson()); }

String toJson() {
    JsonDocument doc;
    doc["version"] = PS_VERSION;
    doc["device_name"] = devName;
    doc["tz"] = tz;

    JsonObject auth = doc["auth"].to<JsonObject>();
    auth["user"] = authUser;
    auth["salt"] = authSalt;
    auth["hash"] = authHash;

    JsonArray arr = doc["zones"].to<JsonArray>();
    for (const ZoneConfig& z : zones) {
        JsonObject zj = arr.add<JsonObject>();
        zj["name"] = z.name;
        zj["enabled"] = z.enabled;
        zj["default_seconds"] = z.defaultSeconds;
        JsonArray ej = zj["entries"].to<JsonArray>();
        for (const Entry& e : z.entries) {
            JsonObject eo = ej.add<JsonObject>();
            eo["days"] = e.days;
            eo["hour"] = e.hour;
            eo["minute"] = e.minute;
            eo["seconds"] = e.seconds;
            eo["enabled"] = e.enabled;
        }
    }
    String out;
    serializeJson(doc, out);
    return out;
}

bool fromJson(const String& body, String& err) {
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, body);
    if (e) { err = String("JSON non valido: ") + e.c_str(); return false; }
    if (!doc["zones"].is<JsonArray>()) { err = "campo zones mancante"; return false; }
    if (doc["zones"].as<JsonArray>().size() != MAX_ZONES) {
        err = String("servono esattamente ") + MAX_ZONES + " zone";
        return false;
    }

    ZoneConfig tmp[MAX_ZONES];
    String tmpTz = doc["tz"] | String(TIMEZONE_FALLBACK);
    String tmpName = doc["device_name"] | String(PS_HOSTNAME);

    uint8_t zi = 0;
    for (JsonObject zj : doc["zones"].as<JsonArray>()) {
        ZoneConfig& z = tmp[zi++];
        z.name = zj["name"] | (String("Zona ") + zi);
        z.enabled = zj["enabled"] | true;
        long defSec = zj["default_seconds"] | 300L;
        if (defSec < 1 || defSec > (long)MAX_ZONE_SECONDS) {
            err = "default_seconds fuori range per zona " + String(zi);
            return false;
        }
        z.defaultSeconds = (uint16_t)defSec;
        z.entries.clear();
        for (JsonObject eo : zj["entries"].as<JsonArray>()) {
            if (z.entries.size() >= MAX_ENTRIES_PER_ZONE) { err = "troppi eventi"; return false; }
            // Validare PRIMA di restringere a uint8_t/uint16_t: "hour": 256
            // diventerebbe 0 e passerebbe come mezzanotte.
            long days    = eo["days"] | 0x7FL;
            long hour    = eo["hour"] | 0L;
            long minute  = eo["minute"] | 0L;
            long seconds = eo["seconds"] | 600L;
            if (days < 1 || days > 0x7F) { err = "days non valido"; return false; }
            if (hour < 0 || hour > 23) { err = "ora non valida"; return false; }
            if (minute < 0 || minute > 59) { err = "minuti non validi"; return false; }
            if (seconds < 1 || seconds > (long)MAX_ZONE_SECONDS) { err = "durata non valida"; return false; }

            Entry en;
            en.days = (uint8_t)days;
            en.hour = (uint8_t)hour;
            en.minute = (uint8_t)minute;
            en.seconds = (uint16_t)seconds;
            en.enabled = eo["enabled"] | true;
            z.entries.push_back(en);
        }
    }

    // commit atomico in memoria
    for (int i = 0; i < MAX_ZONES; ++i) zones[i] = tmp[i];
    tz = tmpTz;
    devName = tmpName;

    JsonObject auth = doc["auth"];
    if (auth["hash"].is<const char*>()) {
        authUser = auth["user"] | String("");
        authSalt = auth["salt"] | String("");
        authHash = auth["hash"] | String("");
    }
    return true;
}

ZoneConfig& zone(uint8_t i) {
    static ZoneConfig dummy;
    if (i < 1 || i > MAX_ZONES) return dummy;
    return zones[i - 1];
}
uint8_t zoneCount() { return MAX_ZONES; }

String tzRule() { return tz; }
void   setTzRule(const String& rule) { tz = rule; }

String deviceName() { return devName; }
void   setDeviceName(const String& name) { devName = name; }

bool setPassword(const String& user, const String& pass) {
    if (user.length() < 1 || pass.length() < 6) return false;
    authUser = user;
    authSalt = randomHex(8);
    authHash = hashPassword(pass, authSalt);
    return true;
}

void clearPassword() {
    authUser = "";
    authSalt = "";
    authHash = "";
}

bool checkPassword(const String& user, const String& pass) {
    if (authHash.isEmpty()) return false;
    return user == authUser && hashPassword(pass, authSalt) == authHash;
}

bool   hasPassword()    { return !authHash.isEmpty(); }
String passwordUser()   { return authUser; }
String passwordHash()   { return authHash; }

} // namespace store
