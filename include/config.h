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
// Scelta: 8 relè sui 8 pin "comodi" D0..D7, I2C RTC sui pin RX/TX.
//
// Relè  IN1..IN8 : D0(16) D1(5) D2(4) D3(0) D4(2) D5(14) D6(12) D7(13)
// I2C   SDA / SCL: RX(3)  / TX(1)
// LED   status   : D8(15)
//
// ATTENZIONE - PIN DI BOOT:
//   D3 (GPIO0) e D4 (GPIO2) sono STRAP: l'ESP li campiona al reset.
//   con RELAY_ACTIVE_LOW (relè riposo = HIGH) gli ingressi opto sono
//   tirati HIGH da pull-up esterni sulla scheda relè, quindi il boot
//   procede, ma possono esserci glitch SUB-MILLISECOND sui relè 4 e 5
//   a ogni avvio. Documentato in README > Limitazioni note.
//
// ATTENZIONE - SERIALE:
//   RX/TX sono occupati dall'I2C => nessuna Serial a runtime.
//   Debug via UDP (porta 54123) e endpoint HTTP /api/debug.
//   Per riflashare da USB, staccare alimentazione e tenere premuto BOOT
//   durante il reset (procedura standard ESP8266).
// ---------------------------------------------------------------------
namespace Pin {
    static const int RELAY[9] = {
        -1,   // IN0 non usato
        16,   // IN1 = D0
        5,    // IN2 = D1
        4,    // IN3 = D2
        0,    // IN4 = D3   (strap GPIO0)
        2,    // IN5 = D4   (strap GPIO2)
        14,   // IN6 = D5
        12,   // IN7 = D6
        13,   // IN8 = D7
    };

    static const int I2C_SDA    = 3;   // RX
    static const int I2C_SCL    = 1;   // TX
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
