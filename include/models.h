#pragma once
#include <Arduino.h>
#include <vector>

// Una singola finestra di irrigazione programmata.
// days: bitmask 0=Lun ... 6=Dom (bit 0 = lunedì)
struct Entry {
    uint8_t  days   = 0x7F;      // default: tutti i giorni
    uint8_t  hour   = 0;
    uint8_t  minute = 0;
    uint16_t seconds = 600;      // durata 10 min
    bool     enabled = true;

    bool matchesDay(uint8_t dow) const { return days & (1u << dow); }
};

struct ZoneConfig {
    String  name;
    uint16_t defaultSeconds = 300;
    std::vector<Entry> entries;
    bool    enabled = true;
};
