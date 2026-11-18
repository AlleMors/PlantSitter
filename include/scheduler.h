#pragma once
#include "models.h"

enum RunReason { RUN_MANUAL, RUN_SCHEDULE };

struct ZoneState {
    bool     open = false;
    uint32_t remainingSec = 0;    // tempo residuo della sessione corrente
    RunReason reason = RUN_MANUAL;
    time_t   endAt = 0;           // epoch di fine (informativo, per la UI)
    uint32_t lastAccountMs = 0;   // millis() dell'ultima contabilizzazione
    uint32_t accMs = 0;           // resto sub-secondo non ancora contabilizzato
    uint32_t todayUsedSec = 0;    // secondi accumulati oggi per la zona
};

// Motore di irrigazione: una zona alla volta, con limiti di sicurezza,
// stato sicuro al boot e autonomia di esecuzione senza rete.
namespace sched {
    void begin();
    void tick();                              // chiamato ogni SCHEDULER_TICK_MS
    bool openZone(uint8_t zone, uint16_t seconds, RunReason reason, String& err);
    bool closeZone(const String& reason);
    void closeAll();

    const ZoneState& state(uint8_t zone);
    int8_t activeZone();                      // -1 se nessuna
    void   resetDailyCounters();              // a mezzanotte
    bool   rtcUsable();
    void   setRtcUsable(bool v);
}
