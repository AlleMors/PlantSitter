#include "config.h"
#include "logger.h"
#include <WiFiUdp.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <time.h>

namespace {
    struct Line {
        char text[LOG_LINE_MAX];
        LogLevel level;
    };

    // Buffer in heap: se l'allocazione fallisce, i log vanno solo su UDP.
    Line*     ring = nullptr;
    size_t    idx = 0;
    size_t    count = 0;
    bool      fsReady = false;
    bool      udpReady = false;
    IPAddress udpPeer;
    WiFiUDP   udp;
    unsigned long lastFileFlush = 0;

    const char* levelStr(LogLevel l) {
        switch (l) {
            case LOG_INFO:  return "INFO";
            case LOG_WARN:  return "WARN";
            case LOG_ERROR: return "ERR ";
            default:        return "DBG ";
        }
    }

    String timestamp() {
        time_t t = time(nullptr);
        if (t < 86400) return String("     --:--:--");
        struct tm tm;
        localtime_r(&t, &tm);
        char buf[24];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return String(buf);
    }

    void push(const String& s, LogLevel lvl) {
        if (!ring) return;
        Line& ln = ring[idx];
        strncpy(ln.text, s.c_str(), LOG_LINE_MAX - 1);
        ln.text[LOG_LINE_MAX - 1] = '\0';
        ln.level = lvl;
        idx = (idx + 1) % LOG_BUFFER_LINES;
        if (count < LOG_BUFFER_LINES) count++;
    }

    void appendFile(const String& s) {
        if (!fsReady) return;
        File f = LittleFS.open(LOG_FILE, "a");
        if (!f) return;
        f.println(s);
        f.close();
    }

}

namespace pslog {

void begin() {
    fsReady = LittleFS.begin();
    // allocazione lazy del ring: se la RAM è bassa, restiamo senza history
    if (!ring) {
        ring = new (std::nothrow) Line[LOG_BUFFER_LINES];
        if (!ring) {
            // niente PS_LOG qui: il logger stesso è il chiamante
        }
    }
}

void setUdpPeer(IPAddress addr) {
    udpPeer = addr;
    udpReady = true;
    udp.begin(0); // porta effimera per risposta
}

void write(LogLevel level, const char* file, int line, const char* fmt, ...) {
    char msg[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    const char* base = strrchr(file, '/');
    base = base ? base + 1 : file;

    String out = timestamp() + "  " + levelStr(level) + " " + base + ":" + String(line) + "  " + msg;

    push(out, level);
    if (udpReady && WiFi.status() == WL_CONNECTED) {
        udp.beginPacket(udpPeer, LOG_UDP_PORT);
        udp.write((const uint8_t*)out.c_str(), out.length());
        udp.endPacket();
    }

    unsigned long now = millis();
    if (level == LOG_ERROR || level == LOG_WARN || now - lastFileFlush > 10000) {
        appendFile(out);
        lastFileFlush = now;
    }
}

String dumpJson(size_t maxLines) {
    if (!ring) return "[]";
    size_t total = count < maxLines ? count : maxLines;
    size_t skip = count - total;
    String out = "[";
    bool first = true;
    for (size_t i = skip; i < count; ++i) {
        const Line& ln = ring[(idx + LOG_BUFFER_LINES - count + i) % LOG_BUFFER_LINES];
        if (!first) out += ",";
        first = false;
        out += "\"";
        for (const char* p = ln.text; *p; ++p) {
            char c = *p;
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (c == '\n') out += "\\n";
            else if ((unsigned char)c < 0x20) out += ' ';
            else out += c;
        }
        out += "\"";
    }
    out += "]";
    return out;
}

String dumpText(size_t maxLines) {
    if (!ring) return "";
    size_t total = count < maxLines ? count : maxLines;
    size_t skip = count - total;
    String out;
    for (size_t i = skip; i < count; ++i) {
        const Line& ln = ring[(idx + LOG_BUFFER_LINES - count + i) % LOG_BUFFER_LINES];
        out += ln.text;
        out += "\n";
    }
    return out;
}

void pruneToFile() {
    if (!fsReady) return;
    File src = LittleFS.open(LOG_FILE, "r");
    if (!src) return;
    size_t sz = src.size();
    if (sz < 64 * 1024) { src.close(); return; }

    // Apriamo il temporaneo PRIMA di toccare l'originale: se una delle due
    // aperture fallisce, non dobbiamo cancellare nulla.
    const char* tmpPath = DATA_DIR "/log.tmp";
    File tmp = LittleFS.open(tmpPath, "w");
    if (!tmp) { src.close(); return; }

    static char buf[1024];
    src.seek(sz / 2);
    size_t written = 0;
    while (src.available()) {
        size_t n = src.readBytes(buf, sizeof(buf));
        if (n == 0) break;
        if (tmp.write((uint8_t*)buf, n) != n) {
            tmp.close(); src.close();
            LittleFS.remove(tmpPath);
            return;                    // originale intatto
        }
        written += n;
    }
    src.close();
    tmp.close();
    if (written == 0) { LittleFS.remove(tmpPath); return; }

    if (LittleFS.rename(tmpPath, LOG_FILE)) return;   // rimpiazzo atomico
    LittleFS.remove(tmpPath);                          // rinomina fallita: originale resta
}

} // namespace pslog
