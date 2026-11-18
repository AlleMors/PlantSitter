#include "config.h"
#include "rtc.h"
#include "logger.h"
#include <Wire.h>
#include <RTClib.h>
#include <ESP8266WiFi.h>
#include <sys/time.h>
#include <time.h>

namespace {
    RTC_DS3231 rtc;
    bool  present = false;
    String tzRule = TIMEZONE_FALLBACK;
    unsigned long lastSync = 0;
    const unsigned long RESYNC_MS = 6UL * 3600UL * 1000UL; // 6 h

    // Conversione indipendente da TZ: giorni dall'epoca (algoritmo civile)
    // + secondi del giorno. L'RTC memorizza UTC.
    time_t toEpoch(const DateTime& dt) {
        int y = dt.year();
        unsigned m = dt.month();
        unsigned d = dt.day();
        y -= m <= 2;
        const int era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        const long days = era * 146097L + static_cast<long>(doe) - 719468L;
        return static_cast<time_t>(days) * 86400L + dt.hour() * 3600L + dt.minute() * 60L + dt.second();
    }

    bool ntpAvailable() {
        time_t t = time(nullptr);
        return t > 1700000000;
    }
}

namespace psrtc {

bool begin() {
    // I2C sui pin scelti (RX/TX)
    Wire.begin(Pin::I2C_SDA, Pin::I2C_SCL);

    // Imposta TZ del sistema a UTC, così mktime/localtime_s sono coerenti
    // finché non arriva NTP.
    setenv("TZ", "UTC0", 1);
    tzset();

    present = rtc.begin(&Wire);
    if (!present) {
        PS_LOG(LOG_ERROR, "DS3231 non rilevato su SDA=%d SCL=%d", Pin::I2C_SDA, Pin::I2C_SCL);
        return false;
    }
    if (rtc.lostPower()) {
        // NON impostare __DATE__/__TIME__: è l'ora di compilazione del
        // firmware, quasi sempre nel passato e senza rapporto con la realtà.
        // Un orologio "plausibile" ma falso farebbe scattare i programmi
        // all'ora sbagliata. Meglio dichiararlo inaffidabile: la
        // schedulazione resta spenta finché NTP non sincronizza.
        PS_LOG(LOG_ERROR, "DS3231: batteria scarica o primo avvio: "
               "orario RTC non attendibile, attesa NTP");
        present = false;
        return false;
    }

    if (!ntpAvailable() && rtcValid()) {
        // Boot senza rete: il tempo di sistema parte dall'RTC.
        struct timeval tv{ (time_t)rtcNow(), 0 };
        settimeofday(&tv, nullptr);
        PS_LOG(LOG_INFO, "orario impostato da RTC (nessun NTP)");
    }

    applyTimezone(tzRule);
    lastSync = millis();
    return true;
}

bool rtcPresent() { return present; }

bool rtcValid() {
    if (!present) return false;
    // plausibilità: dopo il 1/1/2024
    return toEpoch(rtc.now()) > 1704067200;
}

time_t rtcNow() { return toEpoch(rtc.now()); }

void rtcSet(time_t t) {
    if (!present) return;
    rtc.adjust(DateTime(t));
    struct timeval tv{ t, 0 };
    settimeofday(&tv, nullptr);
}

void ntpSync() {
    // NB: configTime(int, int, ...) NON imposta il fuso, scrive un offset
    // fisso e ignora la variabile TZ (vedi cores/esp8266/time.cpp). Va usata
    // la variante con la regola POSIX, altrimenti localtime restituisce UTC
    // e tutti gli orari programmati slittano di una o due ore.
    configTime(tzRule.c_str(), "pool.ntp.org", "time.cloudflare.com");
    unsigned long start = millis();
    while (!ntpAvailable() && millis() - start < 10000) {
        delay(100);
    }
    if (!ntpAvailable()) {
        PS_LOG(LOG_WARN, "NTP non raggiungibile, uso RTC");
        if (present && rtcValid()) {
            struct timeval tv{ (time_t)rtcNow(), 0 };
            settimeofday(&tv, nullptr);
        }
        return;
    }
    if (present) {
        rtc.adjust(DateTime((time_t)time(nullptr)));
    }
    applyTimezone(tzRule);
    lastSync = millis();
    PS_LOG(LOG_INFO, "NTP sincronizzato");
}

void applyTimezone(const String& rule) {
    tzRule = rule;
    setTZ(rule.c_str());   // setenv + tzset, come il core
}

bool clockValid() { return time(nullptr) > 1704067200; }

void loop() {
    // Riallineamento RTC a NTP ogni 6 h (solo se l'ora di sistema è valida
    // e la rete è attiva): protegge dal drift dell'RTC.
    if (present && ntpAvailable() && WiFi.status() == WL_CONNECTED) {
        unsigned long elapsed = millis() - lastSync;
        if (elapsed > RESYNC_MS) {
            rtc.adjust(DateTime((time_t)time(nullptr)));
            lastSync = millis();
            PS_LOG(LOG_DEBUG, "RTC riallineato a NTP");
        }
    }
}

} // namespace psrtc
