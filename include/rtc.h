#pragma once
#include <Arduino.h>

// Orario: DS3231 come riferimento persistente + NTP come correzione.
// Se al boot NTP non risponde entro timeout, si usa l'RTC.
namespace psrtc {
    bool begin();                     // init I2C + RTC
    bool rtcPresent();
    bool rtcValid();                  // ha un orario plausibile
    time_t rtcNow();
    void   rtcSet(time_t t);

    void   ntpSync();                 // sincronizza di nuovo (chiamata periodica)
    void   applyTimezone(const String& tzRule);
    bool   clockValid();              // ora di sistema plausibile
    void   loop();                    // richiamata dal main per re-sync
}
