// =====================================================================
// PlantSitter - main
// =====================================================================
// Ordine di avvio:
//   1. Relè in stato sicuro (tutti chiusi) PRIMA di toccare altro.
//   2. Config e filesystem.
//   3. WiFi (captive portal al primo avvio) + mDNS.
//   4. Orario: NTP, fallback RTC.
//   5. Web server.
// =====================================================================
#include <Arduino.h>
#include "config.h"
#include "logger.h"
#include "store.h"
#include "scheduler.h"
#include "rtc.h"
#include "web.h"

#include <WiFiManager.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>

#ifdef PLANTSITTER_DEBUG_BUILD
#include <SoftwareSerial.h>
#endif

namespace {
    WiFiManager wm;
    bool timeSynced = false;
    bool mdnsStarted = false;
    unsigned long bootMs = 0;

    void configModeCallback(WiFiManager*) {
        PS_LOG(LOG_INFO, "captive portal: SSID %s, IP %s",
               PS_AP_NAME, WiFi.softAPIP().toString().c_str());
    }

    bool connectWiFi() {
        // ssid/pass persistiti da WiFiManager in LittleFS
        wm.setConfigPortalTimeout(180);
        wm.setAPCallback(configModeCallback);
        wm.setBreakAfterConfig(true);
        wm.setSaveConnect(true);

        bool ok = wm.autoConnect(PS_AP_NAME, strlen(PS_AP_PASSWORD) ? PS_AP_PASSWORD : nullptr);
        if (!ok) {
            PS_LOG(LOG_WARN, "WiFi non connesso: il dispositivo resta in modalità AP/setup");
            // Non bloccare: la UI deve restare raggiungibile almeno in AP
            wm.startConfigPortal(PS_AP_NAME, strlen(PS_AP_PASSWORD) ? PS_AP_PASSWORD : nullptr);
        }
        return ok;
    }

    void startMdns() {
        if (mdnsStarted) return;
        if (MDNS.begin(store::deviceName().c_str())) {
            MDNS.addService("http", "tcp", WEB_PORT);
            mdnsStarted = true;
            PS_LOG(LOG_INFO, "mDNS: http://%s.local", store::deviceName().c_str());
        } else {
            PS_LOG(LOG_WARN, "mDNS non avviato");
        }
    }
}

void setup() {
    bootMs = millis();

    // 1) SICUREZZA PRIMA DI TUTTO: sched::begin() scrive i livelli safe
    //    sui relè prima di qualsiasi altra init (WiFi/file system).
    sched::begin();

#ifdef PLANTSITTER_DEBUG_BUILD
    Serial.begin(115200);
    Serial.println("\nPlantSitter debug build");
#endif

    // 2) Filesystem + config. Il logger va inizializzato PRIMA di load():
    //    un config corrotto o un fallback ai default è proprio l'evento che
    //    deve restare nei log, e senza ring quei messaggi andrebbero persi.
    bool fsOk = store::begin();
    pslog::begin();
    if (!fsOk) PS_LOG(LOG_ERROR, "LittleFS non montato");
    if (!store::load()) {
        PS_LOG(LOG_ERROR, "config non caricata: verifica la UI all'avvio");
    }

    // 3) Orario: RTC subito (funziona anche senza rete)
    bool rtcOk = psrtc::begin();
    sched::setRtcUsable(rtcOk);
    psrtc::applyTimezone(store::tzRule());

    // 5) WiFi
    bool connected = connectWiFi();
    if (connected) {
        // 6) NTP con fallback RTC
        psrtc::ntpSync();
        timeSynced = psrtc::clockValid();

        // 7) mDNS
        startMdns();

        // 8) Debug UDP: manda i log all'IP del client che ha fatto la richiesta
        //    (di fatto l'utente è già raggiungibile; usiamo l'IP del gateway).
        pslog::setUdpPeer(WiFi.gatewayIP());
    } else {
        startMdns(); // mDNS in AP mode comunque
    }

    // 9) Web server (in qualsiasi modalità)
    web::begin();

    if (connected) web::onConnected();

    unsigned long elapsed = millis() - bootMs;
    PS_LOG(LOG_INFO, "boot completato in %lu ms", elapsed);
    if (elapsed > SAFE_BOOT_TIMEOUT_MS) {
        PS_LOG(LOG_WARN, "boot lento: %lu ms", elapsed);
    }
}

// Il dispositivo non ha seriale a runtime (RX/TX sui relè): il LED di stato
// è l'unico feedback immediato. Pattern:
//   lampeggio veloce  = modalità setup / WiFi assente
//   doppio lampo      = WiFi ok ma orologio non sincronizzato
//   battito lento     = tutto ok
//   acceso fisso      = zona aperta
void updateStatusLed() {
    static unsigned long last = 0;
    static bool on = false;
    unsigned long now = millis();
    unsigned long phase;
    bool want;

    if (sched::activeZone() > 0) {
        digitalWrite(Pin::STATUS_LED, HIGH);
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        phase = 200UL;                     // 200 ms acceso / 200 spento
        want = ((now % (phase * 2)) < phase);
    } else if (!psrtc::clockValid()) {
        phase = 2000UL;                    // doppio lampo ogni 2 s
        unsigned long p = now % phase;
        want = (p < 120) || (p > 260 && p < 380);
    } else {
        phase = 3000UL;                    // battito: 60 ms ogni 3 s
        want = ((now % phase) < 60);
    }
    digitalWrite(Pin::STATUS_LED, want ? HIGH : LOW);
    (void)last; (void)on;
}

void loop() {
    static unsigned long lastHeal = 0;

    updateStatusLed();

    // Critico: chiude la zona alla durata prevista anche senza rete.
    sched::tick();
    psrtc::loop();
    web::loop();

    if (mdnsStarted) MDNS.update();

    // Reconnect WiFi (non blocca lo scheduler)
    static unsigned long disconnectedSince = 0;
    if (WiFi.status() != WL_CONNECTED) {
        if (disconnectedSince == 0) disconnectedSince = millis();
        if (millis() - lastHeal > 10000) {
            lastHeal = millis();
            PS_LOG(LOG_WARN, "WiFi perso, tentativo di riconnessione");
            WiFi.reconnect();
        }
        // Se per un minuto non si riconnette, riavvia: al boot WiFiManager
        // riapre l'AP di setup. Senza questo il dispositivo resta
        // irraggiungibile finché non lo si collega via USB.
        if (millis() - disconnectedSince > 60000UL) {
            PS_LOG(LOG_ERROR, "WiFi assente da 60s: riavvio per riaprire il "
                   "portale di configurazione");
            delay(100);
            ESP.restart();
        }
    } else {
        disconnectedSince = 0;
    }

    // Re-sync orario dopo riconnessione
    if (WiFi.status() == WL_CONNECTED && !timeSynced) {
        psrtc::ntpSync();
        timeSynced = psrtc::clockValid();
    }

    // manutenzione log su LittleFS
    static unsigned long lastPrune = 0;
    if (millis() - lastPrune > 60UL * 60UL * 1000UL) {
        lastPrune = millis();
        pslog::pruneToFile();
    }

    delay(2);
}
