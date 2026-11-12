#pragma once
// Optional DS3231 real-time clock (Adafruit DS3231 Precision RTC), same RTClib use as
// the MCP fuel station. If no RTC is found the logger still works; the clock is then
// set by the phone or the console and is lost at power-off.
#include <time.h>

namespace rtcclock {
bool begin();              // true if a DS3231 answered; sets the system clock from it
bool present();
bool write(time_t epoch);  // store UTC time in the RTC
}
