#pragma once
#include <Arduino.h>
#include <IPAddress.h>

namespace pslog {
    void begin();
    void write(LogLevel level, const char* file, int line, const char* fmt, ...);
    void setUdpPeer(IPAddress addr);
    // Restituisce le ultime N righe come JSON array (usato da /api/debug e /api/log)
    String dumpJson(size_t maxLines = 100);
    String dumpText(size_t maxLines = 100);
    void pruneToFile();
}
