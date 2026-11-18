#include "config.h"
#include "scheduler.h"
#include "store.h"
#include "logger.h"
#include <time.h>

namespace {
    ZoneState states[MAX_ZONES];
    int8_t    activeIdx = -1;
    bool      rtcOk = false;
    uint32_t  lastTickMs = 0;
    time_t    lastDayKey = 0;
    time_t    firedMinute = 0;
    uint32_t  firedMask = 0;

    // Indice del giorno della settimana (0=Lunedì) da time_t locale.
    int dayOfWeekMon0(time_t t) {
        struct tm tm;
        localtime_r(&t, &tm);
        // tm_wday: 0=domenica..6=sabato
        int w = tm.tm_wday;
        return (w + 6) % 7;
    }

    void writeRelay(uint8_t zone, bool open) {
        if (zone < 1 || zone > MAX_ZONES) return;
        int pin = Pin::RELAY[zone];
        if (pin < 0) return;   // canale disabilitato (es. debug build)
        digitalWrite(pin, open ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL);
    }

    void safeBootLevels() {
        // Livello PRIMA della direzione: pinMode(OUTPUT) abilita il driver con
        // il latch a 0 (= LOW). Con relè attivi bassi sarebbe un impulso di
        // eccitazione di pochi µs su tutti i canali a ogni boot.
        for (int z = 1; z <= MAX_ZONES; ++z) {
            if (Pin::RELAY[z] < 0) continue;
            digitalWrite(Pin::RELAY[z], RELAY_SAFE_LEVEL);
            pinMode(Pin::RELAY[z], OUTPUT);
        }
    }

    bool clockValid() {
        // Soglia unica, coerente con psrtc::clockValid(): 2024-01-01.
        // Valori sotto significano RTC/NTP non sincronizzati.
        return time(nullptr) > 1704067200;
    }

    // True se l'orologio è saltato all'indietro in modo incompatibile con la
    // sessione in corso (correzione NTP, RTC impostato a mano).
    void detectClockJump(time_t now, time_t prevNow) {
        if (now + 5 < prevNow) {
            PS_LOG(LOG_WARN, "orologio arretrato di %ld s: la sessione in corso "
                   "continua a contare con millis()", (long)(prevNow - now));
        }
    }

    void resetDailyIfNeeded(time_t now) {
        struct tm tm;
        localtime_r(&now, &tm);
        // Chiave del giorno dal calendario locale, NON da mktime(): azzerando
        // l'ora e conservando tm_isdst, mktime restituisce epoch diverse per
        // lo stesso giorno a cavallo del cambio d'ora, e il contatore verrebbe
        // azzerato due volte (doppia razione di 4 h).
        time_t dayKey = (time_t)(tm.tm_year * 1000 + tm.tm_yday);
        if (dayKey != lastDayKey) {
            if (lastDayKey != 0) {
                for (auto& s : states) s.todayUsedSec = 0;
            }
            lastDayKey = dayKey;
        }
    }
}

namespace sched {

void begin() {
    // 1) Stato sicuro: TUTTI i relè chiusi prima di qualsiasi altra init.
    safeBootLevels();
    delay(BOOT_RELAY_SETTLE_MS);
    // 2) Lo stato di esecuzione non è mai ripristinato: si parte sempre da chiuso.
    for (auto& s : states) { s = ZoneState{}; }
}

bool rtcUsable() { return rtcOk; }
void setRtcUsable(bool v) { rtcOk = v; }

const ZoneState& state(uint8_t zone) {
    if (zone < 1 || zone > MAX_ZONES) { static ZoneState dummy; return dummy; }
    return states[zone - 1];
}

int8_t activeZone() { return activeIdx; }

bool openZone(uint8_t zone, uint16_t seconds, RunReason reason, String& err) {
    if (zone < 1 || zone > MAX_ZONES) { err = "zona non valida"; return false; }
    ZoneConfig& cfg = store::zone(zone);
    if (!cfg.enabled) { err = "zona disabilitata"; return false; }
    if (seconds == 0) { err = "durata zero"; return false; }
    if (seconds > MAX_ZONE_SECONDS) { err = "durata oltre il massimo consentito"; return false; }
    if (activeIdx >= 0) { err = "una zona è già aperta"; return false; } // mutex anche se non richiesto

    ZoneState& st = states[zone - 1];
    if (st.todayUsedSec + seconds > MAX_DAILY_SECONDS) {
        err = "limite giornaliero della zona superato";
        PS_LOG(LOG_WARN, "blocco apertura zona %u: limite giornaliero", zone);
        return false;
    }

    st.open = true;
    st.remainingSec = seconds;
    st.reason = reason;
    st.endAt = clockValid() ? time(nullptr) + seconds : 0;
    st.lastAccountMs = millis();
    st.accMs = 0;
    activeIdx = zone;
    writeRelay(zone, true);
    PS_LOG(LOG_INFO, "apertura zona %u per %us (%s)", zone, seconds,
           reason == RUN_MANUAL ? "manuale" : "programma");
    return true;
}

bool closeZone(const String& reason) {
    (void)reason;   // usato solo nel log, che è sempre compilato
    if (activeIdx < 0) return false;
    ZoneState& st = states[activeIdx - 1];
    // Contabilizza quanto resta non ancora conteggiato, incluso il resto
    // sub-secondo: sessioni brevissime ripetute non devono sfuggire al
    // limite giornaliero.
    st.accMs += millis() - st.lastAccountMs;
    st.lastAccountMs = millis();
    uint32_t elapsedSec = st.accMs / 1000UL;
    st.accMs -= elapsedSec * 1000UL;
    if (elapsedSec > 0 || st.accMs > 0) {
        // Arrotonda per eccesso: una chiusura manuale sotto il secondo
        // consuma comunque un secondo di budget (lato sicuro).
        st.todayUsedSec += elapsedSec + (st.accMs > 0 ? 1 : 0);
    }
    st.accMs = 0;
    st.open = false;
    st.remainingSec = 0;
    st.endAt = 0;
    writeRelay(activeIdx, false);
    PS_LOG(LOG_INFO, "chiusura zona %d (%s)", activeIdx, reason.c_str());
    activeIdx = -1;
    return true;
}

void closeAll() {
    String r = "closeAll";
    closeZone(r);
    for (int z = 1; z <= MAX_ZONES; ++z) writeRelay(z, false);
}

void tick() {
    uint32_t nowMs = millis();
    if (nowMs - lastTickMs < SCHEDULER_TICK_MS) return;
    lastTickMs = nowMs;

    time_t now = time(nullptr);
    bool valid = clockValid();
    static time_t prevNow = 0;
    if (valid) {
        if (prevNow) detectClockJump(now, prevNow);
        prevNow = now;
        resetDailyIfNeeded(now);
    }

    // 1) La sorgente di verità per la durata è millis(), NON l'orologio di
    //    sistema: una correzione NTP o un RTC impostato a mano possono
    //    spostare time() in avanti o indietro mentre una valvola è aperta.
    //    La sessione termina quando il conto alla rovescia arriva a zero,
    //    a prescindere dalla rete e dall'orologio ("autonomia senza rete").
    if (activeIdx >= 0) {
        ZoneState& st = states[activeIdx - 1];
        // Accumuliamo il tempo reale trascorso, resto compreso: sessioni
        // brevi ripetute devono comunque consumare il budget giornaliero.
        st.accMs += nowMs - st.lastAccountMs;
        st.lastAccountMs = nowMs;

        if (st.accMs >= 1000UL) {
            uint32_t elapsedSec = st.accMs / 1000UL;
            st.accMs -= elapsedSec * 1000UL;
            // Contabilizza anche l'oltrepasso: se il loop è stato bloccato e
            // la valvola è rimasta aperta oltre il termine, quei secondi sono
            // stati consumati davvero.
            st.todayUsedSec += elapsedSec;
            if (elapsedSec >= st.remainingSec) {
                st.remainingSec = 0;
            } else {
                st.remainingSec -= elapsedSec;
            }
        }
        if (st.remainingSec == 0) {
            String r = "durata completata";
            closeZone(r);
        }
    }

    // 2) Avvio degli eventi programmati (solo se orologio valido).
    if (!valid) return;
    struct tm tm;
    localtime_r(&now, &tm);
    const int dow = dayOfWeekMon0(now);
    const time_t minuteKey = now - (now % 60);

    for (uint8_t z = 1; z <= MAX_ZONES; ++z) {
        ZoneConfig& cfg = store::zone(z);
        if (!cfg.enabled) continue;
        for (const Entry& e : cfg.entries) {
            if (!e.enabled) continue;
            if (!e.matchesDay(dow)) continue;
            if (e.hour != tm.tm_hour || e.minute != tm.tm_min) continue;

            // deduplica: nello stesso minuto ogni zona scatta una sola volta
            uint32_t bit = 1UL << (z - 1);
            if (firedMinute != minuteKey) { firedMinute = minuteKey; firedMask = 0; }
            if (firedMask & bit) continue;

            // Con una zona già aperta in quel minuto non accodiamo: saltiamo
            // l'evento e lo segnaliamo (comportamento esplicito, non silenzioso).
            if (activeIdx >= 0) {
                PS_LOG(LOG_WARN, "evento zona %u saltato: zona %d in corso",
                       z, activeIdx);
                firedMask |= bit;
                continue;
            }

            String err;
            if (openZone(z, e.seconds, RUN_SCHEDULE, err)) {
                firedMask |= bit;
            } else {
                PS_LOG(LOG_WARN, "evento zona %u saltato: %s", z, err.c_str());
            }
        }
    }
}

void resetDailyCounters() {
    for (auto& s : states) s.todayUsedSec = 0;
}

} // namespace sched
