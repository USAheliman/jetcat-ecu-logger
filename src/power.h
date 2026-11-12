#pragma once
// Battery build power management (XIAO ESP32-S3 Plus). On the DevKitC test board
// (cfg::HAS_BATTERY false) every function is a harmless no-op and the board never sleeps.
//
// Rules (agreed 5 Nov 2026):
//  - Deep sleep (~34 uA, Seeed) until the ECU's +5 V appears on the wake pin, or the
//    on-board button (GPIO2) or the XIAO B button (GPIO0) is pressed.
//  - Stay awake while the ECU is plugged in, and for cfg::AWAKE_AFTER_MS (10 min) after
//    it is unplugged, after a button / power-on wake, or after the last phone or browser
//    request. Never sleep during a download or a firmware update.
//  - While charging (detected from the battery voltage), stay awake so the LEDs can show it.
//  - Battery below cfg::BATT_CRITICAL_MV: radios off and sleep (wakes again on the ECU,
//    re-checks, and goes straight back to sleep if still critical).
#include <Arduino.h>

namespace power {
void begin();                  // early in setup(): wake reason, RTC power on, battery first read
void poll();                   // from loop()
bool ecuPresent();             // ECU +5 V on the wake pin (always false on the DevKitC)
int  batteryMv();              // -1 = no battery sense (DevKitC, or divider not fitted)
bool batteryLow();
// Charging, worked out from the battery voltage (the XIAO has no charge-status pin):
// a sustained rise means USB is charging it. Detected a few minutes after it starts.
enum class Charge : uint8_t { Unknown, NotCharging, Charging, Full };
Charge charge();
const char* chargeText();
// Battery gauge colour for the LEDs: 2 = green (>~70 %), 1 = yellow, 0 = red (<~30 %), -1 = no reading.
int gaugeLevel();
const char* wakeReason();      // "power-on", "ECU", "button", ...
uint32_t awakeSecondsLeft();   // 0 on the DevKitC (never sleeps)
void noteActivity();           // a phone/browser request: restart the 10-minute timer
void setStayAwake(bool on);    // console "stay": keep awake for bench work (until reboot)
bool stayAwake();
void sleepNow(const char* why);                 // wakes on ECU power or a button
void sleepNow(const char* why, bool ecuWake);   // ecuWake false: a button or 30-min timer only
}
