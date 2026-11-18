#pragma once
#include "models.h"

// Persistenza configurazione su LittleFS (/config.json) con salvataggio
// atomico (file temporaneo + rename) e API per (de)serializzare.
namespace store {
    bool begin();
    bool load();
    bool save();                 // scrive su LittleFS
    String toJson();
    bool fromJson(const String& body, String& err);

    // stato corrente in memoria (rilasciato dallo scheduler)
    ZoneConfig& zone(uint8_t i);
    uint8_t    zoneCount();
    String     tzRule();
    void       setTzRule(const String& rule);
    String     deviceName();
    void       setDeviceName(const String& name);
    bool       setPassword(const String& user, const String& pass);
    void       clearPassword();
    bool       checkPassword(const String& user, const String& pass);
    bool       hasPassword();
    String     passwordUser();
    String     passwordHash();
}
