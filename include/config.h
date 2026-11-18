// =====================================================================
// PlantSitter - configurazione centralizzata
// =====================================================================
// Tutte le decisioni di build (pinout, logica relè, orari, limiti) sono
// qui. Non toccare la logica di src/: adatta solo questo file.
// =====================================================================
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------
// VERSIONE
// ---------------------------------------------------------------------
#define PS_VERSION            "1.0.0"
#define PS_HOSTNAME           "plantsitter"         // mDNS -> plantsitter.local
#define PS_AP_NAME            "PlantSitter-Setup"
#define PS_AP_PASSWORD        ""                    // vuoto = captive portal aperto
#define PS_API_TOKEN_HEADER   "X-Auth-Token"

// ---------------------------------------------------------------------
// PINOUT
// ---------------------------------------------------------------------
// Relè  IN1..IN8 : D0(16) D1(5) D2(4) D5(14) D6(12) D7(13) RX(3) TX(1)
// I2C   SDA / SCL: D3(0) / D4(2)
// LED   status   : D8(15)
//
// PERCHÉ COSÌ: nessun relè sui pin di boot elimina i glitch all'accensione.
// I pin di boot (D3/GPIO0, D4/GPIO2, D8/GPIO15) sono campionati al reset
// prima che il firmware parta: un relè attivo-basso li porterebbe LOW e
// potrebbe eccitare una valvola per qualche istante a ogni riavvio.
// Qui invece li usano I2C e LED, che sono compatibili con i livelli
// richiesti (pull-up del bus HIGH su GPIO0/GPIO2, LED verso GND su GPIO15).
//
// ATTENZIONE - SERIALE:
//   RX/TX sono occupati dai relè 7 e 8 => nessuna Serial a runtime.
//   Debug via UDP (porta 54123) e endpoint HTTP /api/debug.
//   Per riflashare da USB, staccare alimentazione dalla scheda relè e
//   tenere premuto BOOT durante il reset (procedura standard ESP8266).
// ---------------------------------------------------------------------
namespace Pin {
    // Nessun relè sui pin di boot (D3/GPIO0, D4/GPIO2, D8/GPIO15): al reset
    // l'ESP li campiona prima che il firmware parta, e un ingresso relè
    // attivo-basso li ecciterebbe per una frazione di secondo.
    static const int RELAY[9] = {
        -1,   // IN0 non usato
        16,   // IN1 = D0
        5,    // IN2 = D1
        4,    // IN3 = D2
        14,   // IN4 = D5
        12,   // IN5 = D6
        13,   // IN6 = D7
        3,    // IN7 = RX (GPIO3)
#ifdef PLANTSITTER_DEBUG_BUILD
        -1,   // IN8 disabilitato nel build di debug: TX serve alla seriale
#else
        1,    // IN8 = TX (GPIO1)
#endif
    };

    // I2C sui pin di boot: le pull-up del bus (sul modulo DS3231) li tengono
    // HIGH, che è esattamente il livello richiesto al boot. Un ingresso relè
    // attivo-basso, invece, li tirerebbe LOW e bloccherebbe l'avvio.
    static const int I2C_SDA    = 0;   // D3 (GPIO0)
    static const int I2C_SCL    = 2;   // D4 (GPIO2)
    static const int STATUS_LED = 15;  // D8 (strap LOW, LED verso GND = ok)
}

// Scheda relè: quasi tutti i moduli economici con optoisolatore sono
// ATTIVI BASSI (LOW = relè eccitato). DA VERIFICARE col multimetro, poi
// lasciare true o passare a false.
#define RELAY_ACTIVE_LOW      true
#define RELAY_SAFE_LEVEL      (RELAY_ACTIVE_LOW ? HIGH : LOW)
#define RELAY_ACTIVE_LEVEL    (RELAY_ACTIVE_LOW ? LOW  : HIGH)

// ---------------------------------------------------------------------
// RTC DS3231
// ---------------------------------------------------------------------
#define RTC_I2C_ADDRESS       0x68

// ---------------------------------------------------------------------
// LIMITI DI SICUREZZA
// ---------------------------------------------------------------------
#define MAX_ZONE_SECONDS      (60UL * 60UL)       // 60 min per singola sessione
#define MAX_DAILY_SECONDS     (4UL * 3600UL)      // 4 h totali per zona/giorno
#define BOOT_RELAY_SETTLE_MS  50                  // attesa dopo scrittura relè al boot
#define SAFE_BOOT_TIMEOUT_MS  5000                // se l'app non avvia entro 5 s, forceReset

// ---------------------------------------------------------------------
// SCHEDULER
// ---------------------------------------------------------------------
#define MAX_ZONES             8
#define MAX_ENTRIES_PER_ZONE  8                   // 8 finestre per zona
#define SCHEDULER_TICK_MS     1000                // valutazione ogni secondo
#define TIMEZONE_FALLBACK     "CET-1CEST,M3.5.0,M10.5.0/3"

// ---------------------------------------------------------------------
// WEB / API
// ---------------------------------------------------------------------
#define WEB_PORT              80
#define API_BODY_LIMIT        16384
#define SESSION_COOKIE        "ps_sid"
#define SESSION_TTL_SEC       (3600UL * 24UL * 7UL) // 7 giorni
#define SESSION_MAX           4

// ---------------------------------------------------------------------
// LOGGING
// ---------------------------------------------------------------------
#define LOG_UDP_PORT          54123
// Il buffer del logger vive in heap (non in .bss): teniamo conto della RAM
// limitata dell'ESP8266 (~40 KB utili dopo WiFi + stack).
// In produzione i log INFO sono frequenti: la history serve solo per il
// debug, quindi teniamo poche righe e non scriviamo su flash se non WARN+
#define LOG_BUFFER_LINES      60
#define LOG_LINE_MAX          112
// I file di stato NON devono stare nella radice servita dalla web UI:
// /config.json contiene salt e hash della password. LittleFS non ha
// permessi, quindi la separazione è per percorso + handler statici espliciti.
#define DATA_DIR              "/data"
#define LOG_FILE              DATA_DIR "/log.txt"

// I log sono SEMPRE attivi: sono l'unico modo per vedere gli eventi di
// sicurezza (limiti superati, orologio non valido, errori di scrittura)
// su un dispositivo senza seriale a runtime. INFO/DEBUG restano economici,
// il costo vero è solo su WARN/ERROR (che non sono in un loop caldo).
#define PS_LOG(level, fmt, ...) pslog::write(level, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

enum LogLevel { LOG_INFO, LOG_WARN, LOG_ERROR, LOG_DEBUG };
