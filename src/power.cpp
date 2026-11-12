#include "power.h"
#include <WiFi.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include "config.h"
#include "console.h"
#include "logger.h"
#include "net.h"

namespace power {

static const char* reason = "power-on";
static uint32_t awakeUntil = 0;
static bool stay = false;
static int battMv = -1;
static uint32_t lastBattCheck = 0;
static uint8_t criticalCount = 0;
static bool lowReported = false;

static int readBatteryMv() {
    if (cfg::PIN_BATT_ADC < 0) return -1;
    uint32_t sum = 0;
    for (int i = 0; i < 16; ++i) sum += analogReadMilliVolts(cfg::PIN_BATT_ADC);   // calibrated mV
    int mv = (int)((sum / 16) * cfg::BATT_DIVIDER);
    return mv < cfg::BATT_ABSENT_MV ? -1 : mv;
}

static void extend() { awakeUntil = millis() + cfg::AWAKE_AFTER_MS; }

void begin() {
    if (!cfg::HAS_BATTERY) { reason = "USB (test board)"; return; }

    // Pins held low through the last deep sleep: release them.
    for (int p : {cfg::PIN_RTC_POWER, cfg::PIN_LED_POWER, cfg::PIN_LED_CONNECTED, cfg::PIN_LED_TRANSFER})
        if (p >= 0) gpio_hold_dis((gpio_num_t)p);
    gpio_deep_sleep_hold_dis();

    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_EXT0: reason = "ECU"; break;
        case ESP_SLEEP_WAKEUP_EXT1: reason = "button"; break;
        case ESP_SLEEP_WAKEUP_TIMER: reason = "battery re-check timer"; break;
        default: reason = "power-on"; break;
    }
    // The wake pins come back as RTC IO after a wake; make them ordinary inputs again.
    rtc_gpio_deinit((gpio_num_t)cfg::PIN_ECU_WAKE);
    rtc_gpio_deinit((gpio_num_t)cfg::PIN_WAKE_BUTTON);
    rtc_gpio_deinit((gpio_num_t)cfg::PIN_BOOT_BUTTON);
    // No internal pull-down on the ECU wake pin: the board's 150k already holds it low when
    // unplugged, and the chip's own pull-down (45 kOhm typical) in parallel with the 150k
    // dragged the divider's 3.0 V down below the logic-high threshold (0.68 V measured
    // 12 Nov 2026), so plugging in the ECU didn't wake it.
    pinMode(cfg::PIN_ECU_WAKE, INPUT);
    pinMode(cfg::PIN_WAKE_BUTTON, INPUT_PULLUP);
    pinMode(cfg::PIN_BOOT_BUTTON, INPUT_PULLUP);

    // RTC module on (its own cell keeps the time while it is unpowered).
    pinMode(cfg::PIN_RTC_POWER, OUTPUT);
    digitalWrite(cfg::PIN_RTC_POWER, HIGH);
    delay(20);

    analogSetPinAttenuation(cfg::PIN_BATT_ADC, ADC_11db);   // full 0-3.1 V range
    battMv = readBatteryMv();
    lastBattCheck = millis();
    extend();
}

bool ecuPresent() { return cfg::PIN_ECU_WAKE >= 0 && digitalRead(cfg::PIN_ECU_WAKE) == HIGH; }
int batteryMv() { return battMv; }
bool batteryLow() { return battMv > 0 && battMv < cfg::BATT_LOW_MV; }
const char* wakeReason() { return reason; }
void noteActivity() { if (cfg::HAS_BATTERY) extend(); }
void setStayAwake(bool on) { stay = on; extend(); }
bool stayAwake() { return stay; }

uint32_t awakeSecondsLeft() {
    if (!cfg::HAS_BATTERY) return 0;
    int32_t left = (int32_t)(awakeUntil - millis());
    return left > 0 ? left / 1000 : 0;
}

void sleepNow(const char* why) { sleepNow(why, true); }

// ecuWake false: do not wake on the ECU's +5 V. Used when sleeping on a critical battery
// with the ECU still powered, which would otherwise wake the logger again at once, over
// and over. A 30-minute timer then re-checks instead.
void sleepNow(const char* why, bool ecuWake) {
    if (!cfg::HAS_BATTERY) { Con.println("[power] the test board does not sleep"); return; }
    Con.printf("[power] sleeping: %s. Wakes on %s.\n", why,
               ecuWake ? "ECU power or the button" : "the button or in 30 min (ECU still powered)");
    delay(100);                                   // let the console line out
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    // Outputs held low while asleep: RTC unpowered, LEDs off.
    for (int p : {cfg::PIN_RTC_POWER, cfg::PIN_LED_POWER, cfg::PIN_LED_CONNECTED, cfg::PIN_LED_TRANSFER}) {
        digitalWrite(p, LOW);
        gpio_hold_en((gpio_num_t)p);
    }
    gpio_deep_sleep_hold_en();

    // Wake 1: ECU +5 V through the 100k/150k divider (3.0 V, high). The 150k holds the pin
    // low when unplugged; the internal pull-down stays OFF (see begin()).
    if (ecuWake) {
        rtc_gpio_pulldown_dis((gpio_num_t)cfg::PIN_ECU_WAKE);
        rtc_gpio_pullup_dis((gpio_num_t)cfg::PIN_ECU_WAKE);
        esp_sleep_enable_ext0_wakeup((gpio_num_t)cfg::PIN_ECU_WAKE, 1);
    } else {
        esp_sleep_enable_timer_wakeup(30ULL * 60 * 1000000);
    }
    // Wake 2: the on-board button (GPIO2) or the XIAO's B button (GPIO0) pulled low. Both
    // rely on internal RTC pull-ups, so keep the RTC peripheral domain powered even on the
    // timer-only path (ext0 off), otherwise GPIO2 would float and wake it at random.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    uint64_t mask = 0;
    for (int p : {cfg::PIN_WAKE_BUTTON, cfg::PIN_BOOT_BUTTON}) {
        if (p < 0) continue;
        rtc_gpio_pullup_en((gpio_num_t)p);
        rtc_gpio_pulldown_dis((gpio_num_t)p);
        mask |= 1ULL << p;
    }
    if (mask) esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);

    Serial.flush();
    esp_deep_sleep_start();
}

void poll() {
    if (!cfg::HAS_BATTERY) return;
    uint32_t now = millis();

    if (now - lastBattCheck >= cfg::BATT_CHECK_MS) {
        lastBattCheck = now;
        battMv = readBatteryMv();
        if (batteryLow() && !lowReported) { Con.printf("[power] battery low: %d mV - charge soon\n", battMv); lowReported = true; }
        criticalCount = (battMv > 0 && battMv < cfg::BATT_CRITICAL_MV) ? criticalCount + 1 : 0;
    }

    // Anything that must keep the logger awake restarts the 10-minute timer.
    bool busy = g_downloading || net::otaBusy();
    if (ecuPresent() || busy || stay || net::bleConnected()) extend();

    if (busy) return;   // never sleep mid-download or mid-update
    if (criticalCount >= 3) { sleepNow("battery critical", !ecuPresent()); return; }
    if ((int32_t)(now - awakeUntil) >= 0) sleepNow("nothing to do for 10 min");
}

}  // namespace power
