# JetCat ECU Logger — phase 3a (DevKitC test build: Bluetooth, WiFi, OTA, remote console)

Plugs into a JetCat ECU's 6-pin RS232 "phone" socket after a flight. It reads the run
summary and the 1,000-second log, stores each flight on flash, and serves the flights
to a phone or browser over **Bluetooth** or **home WiFi**. Firmware can be updated over
either, so the USB lead is only needed for the first flash or a recovery.

Protocol source: JetCat "Serial interface of the JetCat ECU" v1.13, 26 Sep 2006.
Tested against a real dump from ECU V6 firmware 6.42, serial 3934 (BO 105), 1 Nov 2026.

## Phone app (Android)

`android/JetCatLogger`: shows these same pages over Bluetooth, keeps a copy of every
flight on the phone, and can upload them to OneDrive. GitHub builds it; see
`android/JetCatLogger/README.md`.

## Credits and licence

The Bluetooth transport (`src/ble_bridge.h`) is adapted from Malcolm Messiter's
LockDownRadioControl RXV2 receiver (https://github.com/Mmessiter/LDRC_V2), used with
his permission. It carries requests over a Bluetooth characteristic and runs them
through the same handlers as the WiFi web server. The OTA upload scripts follow the
MCP fuel station. Because the bridge is GPL-3.0-or-later, so is this project (`LICENSE`).

## Read-only by design

The only transmit path is `EcuLink::send(Cmd)`, and `Cmd` contains just
`RTY RI1 RA1 RFI RRC RS1 RAC XLO`. There is no code path that can send a write
(`W..`), turbine control (`TCO`) or keyboard (`WKY`/`KEN`) command. Nothing on the
web pages or over Bluetooth can send anything to the ECU except a request to repeat
that same read-only download.

## Wiring (ESP32-S3 DevKitC-1 N16R8)

| ECU socket pin | Signal | ESP32-S3 |
| --- | --- | --- |
| 1 | TxD (3.3 V TTL, measured) | GPIO18 (RX) |
| 6 | RxD | GPIO17 (TX) |
| 5 | GND | GND |
| 2 | +5 V | **not connected** (board powered from USB) |
| 3, 4 | Data bus | **never connect** |

LEDs, each through a 1 kΩ resistor to GND: Power (red) = GPIO4, Connected (yellow) =
GPIO5, Transfer (green) = GPIO6.

RTC (DS3231, optional for this test): SDA = GPIO8, SCL = GPIO9,
VIN = 3V3, GND = GND. Fit the CR1220. Without it the clock is set by the phone or
browser each time the page opens.

All pins are set in `src/config.h`.

## ⚠ This update erases the stored flights

The partition table has changed to make room for two firmware slots (needed for
over-the-air updates). Flashing this build wipes flash storage, including flight
3934-0555. The ECU still holds its log, so plug it in again afterwards and the
flight is downloaded again.

## Build and flash (first time, over USB)

1. Open the folder in VS Code with PlatformIO. The first build downloads the
   NimBLE-Arduino and RTClib libraries.
2. Plug the board into its **CH340 UART** port (right-hand port).
3. Choose the `esp32s3_n16r8` environment and press **Upload**, then open the serial
   monitor (115200). The first boot formats storage, which takes a few seconds.

Expected boot lines:

- `[rtc] DS3231 OK, clock set to …` (or `no DS3231 found` if it is not fitted)
- `[ota] running from ota_0, image state …`
- `[ble] advertising as 'JetCat-Logger'`

## Workshop test

1. **Download.** Power the ECU and plug in. Same output as phase 2, ending
   `[dl] saved 3934-0555 (log full, 1000 rows)`.
2. **WiFi.** In the monitor type `wifi <network> <password>`, for example
   `wifi Workshop mypassword`. If the name has spaces, put it in quotes:
   `wifi "My Network" mypassword`. Then type `net`, which shows the address. Open
   `http://jetcat-logger.local` (or the IP address) on a phone or PC on the same WiFi.
   `wifi off` forgets the network.
3. **Web pages.** The page sets the logger's clock from the phone or PC as soon as it
   opens. Check each tab:
   - **Fleet:** one card per ECU serial.
   - **Flights:** list of flights; tap one for the summary and the RPM, EGT, pump and
     battery charts.
   - **Trends:** each key figure plotted against the ECU's run count, with a
     band showing the usual range. Points outside the band are circled once 6 flights
     are stored.
   - **Settings:** name ECU 3934 "BO 105", run a download, export CSV.
4. **Bluetooth.** With a BLE scanner app (e.g. nRF Connect), check that
   `JetCat-Logger` is advertising. The phone app that uses it comes next.
5. **OTA over WiFi.** Change something harmless, then upload either way:
   - choose the `esp32s3_ota` environment and press **Upload**, or
   - open `http://jetcat-logger.local/ota` and upload
     `.pio/build/esp32s3_n16r8/firmware.bin`.

   The logger reboots into the new firmware. The monitor shows it running from the
   other slot (`ota_1`). If `.local` names don't work on your PC, put the IP address in
   `upload_port` in `platformio.ini`.
6. **USB still wins.** A normal USB upload always boots the firmware just written,
   because `clear_otadata.py` resets the slot choice afterwards.

A firmware update is refused while a download from the ECU is in progress.

## Console commands

`help` · `status` · `ls` · `json <key>` · `csv <key>` · `raw <key>` · `rm <key>` ·
`rmall YES` · `time <epoch>` · `dl` · `wifi <ssid> <password>` · `wifi off` · `net`

### Remote console

Everything the logger prints also goes into a 64 KB buffer in PSRAM (roughly the last
1,000 lines), so the console works without the USB lead:

- **Console tab** on the web page (WiFi or Bluetooth): live output, and a box that takes
  the same commands as the serial monitor.
- `GET /api/console?since=N` returns the output after byte N (`next` is the N to ask for
  next time); `POST /api/console?cmd=status` runs a command.

Lines in the buffer start with the UTC time once the clock is set, otherwise the time
since boot (`+12.3s`). WiFi passwords are shown as `****` in the echoed command. Remote
commands run through the same handler as typed ones, so they can only trigger the same
read-only ECU download.

## API (WiFi and Bluetooth)

| Route | |
| --- | --- |
| `GET /api/status.json` | State, download progress, clock, storage, WiFi/BLE, firmware slot |
| `GET /api/flights.json` | Summaries of every stored flight |
| `GET /api/flight.json?key=` / `flight.csv?key=` | One flight's summary or log |
| `GET /api/helis.json`, `POST /api/heli?serial=&name=` | Heli names by ECU serial |
| `POST /api/time?epoch=` | Set the clock (only if unset or more than 2 s out) |
| `POST /api/download` | Repeat the read-only download now |
| `POST /api/delete?key=` | Delete one flight |
| `POST /api/wifi?ssid=&pass=` | Save home WiFi |
| `GET /api/console?since=`, `POST /api/console?cmd=` | Remote console: recent output, run a command |
| `/api/bleota/begin, status, end, reboot` | Firmware update over Bluetooth (same routes as RXV2) |
| `GET /ota`, `POST /api/ota` | Firmware update over WiFi |

## Stored files

| File | Contents |
| --- | --- |
| `/flights/<key>.json` | Summary, time stamps, key points of the last run, every HS reply |
| `/flights/<key>.csv` | Most recent run from the log, oldest row first |
| `/flights/<key>.raw` | Complete XLO dump exactly as received |
| `/config/helis.txt` | `serial=name` lines |

Key = `<ECU serial>-<runs OK>`. The ECU is fixed to its turbine, so the serial number
identifies the heli. A gap in run numbers means a flight was not downloaded.

Time stamps are back-calculated: real time = download time − (ECU system time − row Time).
They are only used if the last log row is within 10 s of the ECU's system time
(`log_gap_s`), i.e. the ECU has not been switched off since the flight. Otherwise the
times are left empty and `time_source` says why. `session_values_valid` follows the
same test, because RA1 pump min/max and RRC failsafe counters reset at ECU power-up.

Per-flight trend figures (`last_run`): `start_s` (ignition to run), `peak_start_egt_c`,
`max_egt_c`, `flight_s`, and means over the time in Run at full throttle from 30 s
after reaching Run: `mean_flight_rpm`, `mean_flight_egt_c`, `mean_flight_pump_v`.
A rising mean pump voltage at the same RPM is the classic early sign of a tired pump
or a fuel restriction.

## Firmware rollback

The firmware marks itself good after running for 60 s. If the bootloader was built
with rollback support, a new firmware that crashes before then is replaced by the
previous one at the next reset. If the bootloader lacks it, the call does nothing and a
USB upload is the recovery path. Either way a failed or interrupted transfer never
touches the running firmware.

## XIAO ESP32-S3 Plus battery build (env `xiao_plus`) — written, not yet tested on the board

Same firmware, built with `-DBOARD_XIAO_PLUS`. The XIAO Plus has 16 MB flash and 8 MB
PSRAM, so it uses the same partition table as the DevKitC.

### Wiring

Everything is on one 4 × 6 cm proto board: PCB RJ12 jack, push button, LEDs, RTC socket
and battery header. Layout, build steps and the full circuit are in
`JetCat_Logger_XIAO_board_v3.pdf` (replaces the earlier wiring and proto-board PDFs; v3 = the Sourcingmap RJ12 jack).

| Function | XIAO pin | GPIO | Parts |
| --- | --- | --- | --- |
| ECU pin 1 (TxD) → logger RX | D7 | 44 | 1 kΩ in series |
| Logger TX → ECU pin 6 (RxD) | D9 | 8 | 1 kΩ in series |
| ECU pin 5 (GND) | GND | – | |
| ECU pin 2 (+5 V) → wake | D0 | 1 | 100 kΩ from pin 2 to D0, 150 kΩ from D0 to GND (3.0 V) |
| ECU pins 3, 4 | – | – | **never connect** (jack legs cut off) |
| Push button (wake) | D1 | 2 | to GND, internal pull-up |
| Battery sense | D2 | 3 | BAT+ → 1 MΩ → D2 → 1 MΩ → GND, 100 nF D2 → GND |
| DS3231 VCC | D3 | 4 | powered only while awake |
| DS3231 SDA / SCL | D4 / D5 | 5 / 6 | |
| Red LED | D6 | 43 | 1 kΩ to the LED, cathode to GND |
| Yellow / green LED | D8 / D10 | 7 / 9 | each via 1 kΩ, cathode to GND |
| Battery | BAT+ / BAT− back pads | – | fly wires to the battery header; 1S LiPo with protection |

D2 (GPIO3) is a strapping pin, but only for the JTAG source and only when eFuse
STRAP_JTAG_SEL is burnt (default 0, ESP32-S3 datasheet §3.4), so the divider on it is
harmless. D6 (GPIO43) is UART0 TX: the boot ROM prints on it at power-up (§3.3), so the
red LED flickers briefly at start; it never goes to the ECU. 5V and 3V3 unused. Fit the
external antenna.

### How it behaves

- **Asleep** (about 34 µA, Seeed's figure) until the ECU's +5 V appears on D0 or the
  push button (D1) or the XIAO's own **B** button is pressed. (B held at power-up
  starts the XIAO in programming mode; the push button has no such effect.)
- **Awake** while the ECU is plugged in and powered, then for **10 minutes** after it is
  unplugged, after a button wake, or after the last phone or browser request. Never
  sleeps during a download or a firmware update.
- **Bluetooth** is on whenever it is awake. **WiFi** looks for the home network for
  15 s after waking, then switches off until the next wake if it is not found.
- **Battery** read every 10 s. Below 3.5 V the red LED flashes slowly. Below 3.3 V
  (three readings in a row, not during a download) it switches the radios off and
  sleeps. If the ECU is still powered at that point it does not wake on the ECU, only on
  a button or a 30-minute re-check timer, so it cannot get stuck waking and sleeping.
- Console: `power` (battery, wake reason, time left), `sleep` (sleep now),
  `stay on` / `stay off` (keep awake for bench work, until reboot).

### First flash

1. Plug the XIAO's USB-C into the PC. Choose `xiao_plus` and press **Upload**. If the
   upload cannot connect, hold **B**, press and release **R**, release **B**, and upload
   again.
2. Open the monitor. The console is the XIAO's own USB port, so it appears as a
   different COM port from the DevKitC.
3. `wifi <network> <password>`, then `net` for its IP address. Reserve that address in
   the router and put it in `upload_port` under `[env:xiao_plus_ota]`.

### Bench checks still to do

- `power` shows a battery voltage within about 0.05 V of a multimeter on BAT+.
- `sleep`, then press the push button: it should wake with `woke by button`. Repeat
  with **B**.
- Asleep and untouched for an hour on the timer path (`power` after a critical-battery
  sleep): no spurious button wakes.
- Apply 5 V to the 100k/150k divider (or plug into the ECU) while asleep: it should wake
  with `woke by ECU`.
- With USB unplugged, deep-sleep current from the battery.

## Not in this build yet

The Android app (a thin shell around these same pages, forked from Malcolm's RXV2App).

## Version numbering

`FW` in `src/main.cpp` is `"dev"`. Version numbers are left to Mehul.
