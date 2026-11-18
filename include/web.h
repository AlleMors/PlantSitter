#pragma once
#include <Arduino.h>

namespace web {
    void begin();
    void loop();
    // chiamata dalla WiFi manager quando si connette: logga l'IP
    void onConnected();
}
