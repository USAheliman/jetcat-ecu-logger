#include "rtc_clock.h"
#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>
#include <sys/time.h>
#include "config.h"
#include "console.h"

namespace rtcclock {

static RTC_DS3231 rtc;
static bool ok = false;

bool begin() {
    Wire.begin(cfg::PIN_I2C_SDA, cfg::PIN_I2C_SCL);
    ok = rtc.begin(&Wire);
    if (!ok) { Con.println("[rtc] no DS3231 found - clock comes from the phone or console"); return false; }
    if (rtc.lostPower()) {
        Con.println("[rtc] DS3231 lost power (fit/replace the CR1220) - time not valid until set");
        return true;
    }
    DateTime now = rtc.now();           // stored as UTC
    struct timeval tv = {(time_t)now.unixtime(), 0};
    settimeofday(&tv, nullptr);
    Con.printf("[rtc] DS3231 OK, clock set to %04d-%02d-%02dT%02d:%02d:%02dZ\n", now.year(), now.month(),
                  now.day(), now.hour(), now.minute(), now.second());
    return true;
}

bool present() { return ok; }

bool write(time_t epoch) {
    if (!ok) return false;
    rtc.adjust(DateTime((uint32_t)epoch));
    return true;
}

}  // namespace rtcclock
