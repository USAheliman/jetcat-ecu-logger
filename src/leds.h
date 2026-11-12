#pragma once
// Three status LEDs, non-blocking. Call update() often.
#include <Arduino.h>
#include "config.h"

enum class LedMode : uint8_t { Off, On, SlowFlash, FastFlash };

class Leds {
public:
    void begin() {
        for (int p : pins_) { pinMode(p, OUTPUT); write(p, false); }
    }
    void set(LedMode power, LedMode connected, LedMode transfer) {
        modes_[0] = power; modes_[1] = connected; modes_[2] = transfer;
        update();
    }
    void update() {
        uint32_t t = millis();
        bool slow = (t / 500) % 2 == 0;   // 1 Hz
        bool fast = (t / 125) % 2 == 0;   // 4 Hz
        for (int i = 0; i < 3; ++i) {
            bool on = false;
            switch (modes_[i]) {
                case LedMode::Off: on = false; break;
                case LedMode::On: on = true; break;
                case LedMode::SlowFlash: on = slow; break;
                case LedMode::FastFlash: on = fast; break;
            }
            write(pins_[i], on);
        }
    }
private:
    static void write(int pin, bool on) { digitalWrite(pin, (on == cfg::LED_ACTIVE_HIGH) ? HIGH : LOW); }
    const int pins_[3] = {cfg::PIN_LED_POWER, cfg::PIN_LED_CONNECTED, cfg::PIN_LED_TRANSFER};
    LedMode modes_[3] = {LedMode::On, LedMode::Off, LedMode::Off};
};
