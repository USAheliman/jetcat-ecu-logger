#pragma once
#include <stdint.h>
#include <stddef.h>

// All tunable values in one place. Pins avoid GPIO33-37 (octal PSRAM on both boards).
// Board chosen by build flag: -DBOARD_XIAO_PLUS for the XIAO, otherwise the DevKitC.
namespace cfg {

#if defined(BOARD_XIAO_PLUS)
// ---------------------------------------------------------------- Seeed XIAO ESP32-S3 Plus
// Battery build. Pin sources: Seeed XIAO ESP32-S3 wiki (pin map, Plus back pads) and
// Espressif ESP32-S3 GPIO reference (RTC GPIOs 0-21 can wake from deep sleep; GPIO2 =
// ADC1_CH1, GPIO3 = ADC1_CH2). GPIO3 is a strapping pin, but only for the JTAG source and
// only if eFuse STRAP_JTAG_SEL is burnt (default 0, datasheet 3.4), so a divider on it is
// harmless. GPIO43 is UART0 TX: the boot ROM prints on it at power-up, so it drives an LED
// (a brief flicker at start) and never the ECU. All parts sit on the proto board (6 Nov 2026).
constexpr int PIN_ECU_RX = 44;          // D7  <- ECU pin 1 (TxD), 1 kOhm in series
constexpr int PIN_ECU_TX = 8;           // D9  -> ECU pin 6 (RxD), 1 kOhm in series
                                        // ECU pin 5 = GND. Pins 3, 4 NEVER connected.
constexpr int PIN_ECU_WAKE = 1;         // D0  <- ECU pin 2 (+5 V) via 100k, 150k to GND (3.0 V)
constexpr int PIN_WAKE_BUTTON = 2;      // D1  on-board push button to GND (internal pull-up)
constexpr int PIN_BOOT_BUTTON = 0;      // XIAO's own BOOT button "B", also wakes it
constexpr int PIN_BATT_ADC = 3;         // D2  BAT+ -> 1M -> pin -> 1M -> GND, 100 nF to GND
constexpr float BATT_DIVIDER = 2.0f;
constexpr int PIN_RTC_POWER = 4;        // D3  -> DS3231 VCC, on only while awake

constexpr int  PIN_LED_POWER     = 43;  // D6  red
constexpr int  PIN_LED_CONNECTED = 7;   // D8  yellow
constexpr int  PIN_LED_TRANSFER  = 9;   // D10 green
constexpr bool LED_ACTIVE_HIGH   = true;

constexpr int PIN_I2C_SDA = 5;          // D4
constexpr int PIN_I2C_SCL = 6;          // D5

constexpr bool HAS_BATTERY = true;
#else
// ---------------------------------------------------------------- ESP32-S3 DevKitC-1 (test board)
// ECU serial: 3.3 V TTL measured on ECU pin 1 (1 Nov 2026), so direct connection.
constexpr int PIN_ECU_RX = 18;          // ESP32 RX  <- ECU phone-socket pin 1 (TxD)
constexpr int PIN_ECU_TX = 17;          // ESP32 TX  -> ECU phone-socket pin 6 (RxD)
                                        // ECU pin 5 = GND. Pins 2, 3, 4 NOT connected.
constexpr int PIN_ECU_WAKE = -1;        // not used: powered from USB, never sleeps
constexpr int PIN_WAKE_BUTTON = -1;
constexpr int PIN_BOOT_BUTTON = -1;
constexpr int PIN_BATT_ADC = -1;
constexpr float BATT_DIVIDER = 2.0f;
constexpr int PIN_RTC_POWER = -1;       // RTC powered from 3V3

// LEDs (via ~1 kOhm resistors to GND)
constexpr int  PIN_LED_POWER     = 4;
constexpr int  PIN_LED_CONNECTED = 5;
constexpr int  PIN_LED_TRANSFER  = 6;
constexpr bool LED_ACTIVE_HIGH   = true;

// I2C for the DS3231 RTC (ESP32-S3 DevKitC default Wire pins)
constexpr int PIN_I2C_SDA = 8;
constexpr int PIN_I2C_SCL = 9;

constexpr bool HAS_BATTERY = false;
#endif

constexpr uint32_t ECU_BAUD = 9600;     // JetCat default; we never change it
constexpr uint8_t  ECU_ADDR = 1;        // default slave address

// Power rules (battery build only; agreed with Mehul 5 Nov 2026)
constexpr uint32_t AWAKE_AFTER_MS     = 10UL * 60 * 1000;  // awake 10 min after the ECU is unplugged,
                                                           // after a button wake, or after the last phone/browser request
constexpr uint32_t WIFI_TRY_MS        = 15000;   // look for home WiFi this long after waking, then WiFi off
constexpr uint32_t BATT_CHECK_MS      = 10000;   // battery read interval
constexpr int      BATT_ABSENT_MV     = 2500;    // below this the divider is not fitted / no reading: ignore
constexpr int      BATT_LOW_MV        = 3500;    // red LED flashes, "low" in status
constexpr int      BATT_CRITICAL_MV   = 3300;    // radios off and deep sleep (not during a download)
                                                 // The pack's protection circuit is the last line of defence.

// Names on the air
constexpr const char* DEVICE_NAME = "JetCat-Logger";   // Bluetooth name
constexpr const char* MDNS_NAME   = "jetcat-logger";   // http://jetcat-logger.local on home WiFi

// Bluetooth is on whenever the logger is awake (both boards). WiFi: see WIFI_TRY_MS.
constexpr bool RADIOS_ALWAYS_ALLOWED = true;

// Timing
constexpr uint32_t PROBE_INTERVAL_MS    = 2000;    // RTY every 2 s while idle
constexpr uint32_t REPLY_TIMEOUT_MS     = 1000;    // summary commands
constexpr uint32_t XLO_FIRST_ROW_MS     = 30000;   // ECU pauses after the header: >3 s on 1 Nov, >5 s on 5 Nov 2026
constexpr uint32_t XLO_GAP_TIMEOUT_MS   = 5000;    // silence between rows that ends a dump
constexpr uint32_t DRAIN_QUIET_MS       = 20000;   // quiet time that means the ECU has finished sending. Must be
                                                   // longer than its pause after the header, or a retry is sent
                                                   // mid-dump (5 Nov 2026: 3 s was too short)
constexpr uint32_t XLO_TOTAL_TIMEOUT_MS = 120000;  // hard cap (measured dump: 48 s)
constexpr uint32_t COOLDOWN_MAX_MS      = 600000;  // keep asking for the log for up to 10 min
constexpr uint32_t XLO_RETRY_MS         = 10000;   // while the ECU refuses it (running / cooling down)
constexpr size_t   XLO_EMPTY_MAX        = 1024;    // a dump this small with no rows = refused (V6 sends 164 B while cooling)
constexpr uint32_t UNPLUG_TIMEOUT_MS    = 10000;   // no reply this long = unplugged
constexpr long     SAME_SESSION_MAX_GAP_S = 10;    // last log row this close to ECU system time = same power-on
                                                   // session as the flight (to be confirmed at the field)

constexpr size_t XLO_BUF_SIZE = 96 * 1024;         // measured dump 46 KB; held in PSRAM
constexpr size_t CONSOLE_BUF_SIZE = 64 * 1024;     // recent console output for /api/console; PSRAM

}  // namespace cfg
