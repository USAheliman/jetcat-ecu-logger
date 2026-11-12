// JetCat ECU Logger - phase 3a (DevKitC test build)
//
// Plug into the ECU's 6-pin RS232 "phone" socket after a flight. The unit:
//   1. probes with RTY until the ECU answers            (Connected LED on)
//   2. reads the summary: RTY RI1 RA1 RFI RRC RS1 RAC   (Transfer LED flashing)
//   3. dumps the log with XLO (~50 s at 9600 baud)
//   4. saves summary + last run + raw dump to flash      (Transfer LED solid)
// Flights are viewed on the phone over Bluetooth, or in a browser on home WiFi
// (same pages, same API - see net.cpp and ble_bridge.h), or on the USB console.
// Not in this build yet: battery/ECU power detection (XIAO build), the phone app.
//
// Copyright (C) 2026 Mehul Popat. GPL-3.0-or-later (see LICENSE). The Bluetooth
// bridge is adapted from Malcolm Messiter's LockDownRadioControl, with his permission.

#include <Arduino.h>
#include <sys/time.h>
#include <time.h>
#include "config.h"
#include "console.h"
#include "power.h"
#include "ecu_link.h"
#include "leds.h"
#include "logger.h"
#include "net.h"
#include "rtc_clock.h"
#include "store.h"
#include "xlo_parse.h"

const char* FW = "dev";   // version numbering left to Mehul

static Leds leds;
static EcuLink ecu;
static char* xloBuf = nullptr;

enum class State { Idle, Download, Saved, Fault };
static State state = State::Idle;
String g_lastResult = "none yet";
volatile bool g_downloading = false;
volatile bool g_downloadRequested = false;
size_t g_dumpBytes = 0;

// Called while waiting on the ECU: keep LEDs flashing and keep answering the phone.
static void ledsIdle()     { leds.update(); net::poll(); }
// Red LED: steady = power, slow flash = battery low (battery build).
static LedMode pwrLed()    { return power::batteryLow() ? LedMode::SlowFlash : LedMode::On; }
static void showIdle()     { leds.set(pwrLed(), LedMode::Off, LedMode::Off); }
static void showDownload() { leds.set(pwrLed(), LedMode::On, LedMode::SlowFlash); }
static void showSaved()    { leds.set(pwrLed(), LedMode::On, LedMode::On); }
static void showFault()    { leds.set(LedMode::FastFlash, LedMode::FastFlash, LedMode::FastFlash); }

bool clockValid() { return time(nullptr) > 1700000000; }
bool rtcPresent() { return rtcclock::present(); }

static String isoTime(time_t t) {
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char b[24];
    strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return String(b);
}

void setClock(time_t epoch, const char* source) {
    struct timeval tv = {epoch, 0};
    settimeofday(&tv, nullptr);
    bool rtcOk = rtcclock::write(epoch);
    Con.printf("[clock] set to %s by %s%s\n", isoTime(epoch).c_str(), source, rtcOk ? " (RTC updated)" : "");
}

const char* stateName() {
    switch (state) {
        case State::Idle: return "Idle";
        case State::Download: return "Download";
        case State::Saved: return "Saved";
        case State::Fault: return "Fault";
    }
    return "?";
}

static String jsonStr(const String& s) {
    String o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((uint8_t)c < 0x20) o += ' ';
        else o += c;
    }
    return o + "\"";
}

static String param(const Reply& r, size_t i) { return i < r.params.size() ? r.params[i] : String(""); }

// ---------------------------------------------------------------- download
static bool doDownload() {
    showDownload();
    Con.println("[dl] reading summary");

    Reply rty = ecu.query(Cmd::RTY);
    if (rty.timeout || !rty.ok) { g_lastResult = "RTY failed: " + (rty.timeout ? String("timeout") : rty.code); return false; }
    Reply ri1 = ecu.query(Cmd::RI1);
    // Clock read at the same moment as the ECU system time, so back-calculated
    // flight times are not shifted by the ~50 s log dump that follows.
    time_t now = time(nullptr);
    if (ri1.timeout || !ri1.ok) { g_lastResult = "RI1 failed"; return false; }
    Reply ra1 = ecu.query(Cmd::RA1);
    Reply rfi = ecu.query(Cmd::RFI);
    Reply rrc = ecu.query(Cmd::RRC);
    Reply rs1 = ecu.query(Cmd::RS1);
    Reply rac = ecu.query(Cmd::RAC);   // engine state at download (8 = cooling down)

    String serial = param(rty, 4);
    long runsOk = param(ri1, 0).toInt();
    long sysTime = (long)param(ri1, 4).toFloat();
    char keyBuf[40];
    snprintf(keyBuf, sizeof(keyBuf), "%s-%04ld", serial.c_str(), runsOk);
    String key(keyBuf);
    Con.printf("[dl] ECU serial %s, runs OK %ld, ECU system time %ld s -> key %s\n",
                  serial.c_str(), runsOk, sysTime, key.c_str());

    if (store::exists(key)) {
        g_lastResult = "duplicate: " + key + " already stored";
        Con.println("[dl] " + g_lastResult);
        return true;
    }

    // Log dump. While the turbine is running or cooling down the ECU refuses the log: some
    // reply NA; the V6 (Jet-tronic 6.42, 10 Nov 2026) replies OK with an empty log whose only
    // line is "Command not allowed, Turbine is running or in cool down sequence !". Engine
    // state 0 (RAC) is not a reliable end of that: on 10 Nov it showed 0 at ECU time 154 s,
    // XLO was still refused at 155 s and worked at 228 s. So ask for the log again every
    // XLO_RETRY_MS until it comes, for at most COOLDOWN_MAX_MS. Each refusal is instant.
    size_t len = 0;
    bool sawEnd = false;
    auto refused = [&](const Reply& x) {
        if (x.code == "NA") return true;
        return x.ok && sawEnd && len < cfg::XLO_EMPTY_MAX && parseXlo(xloBuf, len).rowCount == 0;
    };
    Con.println("[dl] requesting XLO log (~48 s)");
    Reply xlo = ecu.dumpLog(xloBuf, cfg::XLO_BUF_SIZE, len, sawEnd);
    if (refused(xlo)) {
        String t(xloBuf); t.replace("\r", ""); t.replace("\n", " | ");
        Con.printf("[dl] log refused (%s): %s\n", xlo.code.c_str(), t.substring(0, 240).c_str());
        Con.printf("[dl] turbine running or cooling - asking again every %lu s\n",
                   (unsigned long)(cfg::XLO_RETRY_MS / 1000));
        uint32_t t0 = millis();
        int tries = 1;
        while (refused(xlo) && millis() - t0 < cfg::COOLDOWN_MAX_MS) {
            uint32_t w = millis();
            while (millis() - w < cfg::XLO_RETRY_MS) { leds.update(); delay(10); }
            xlo = ecu.dumpLog(xloBuf, cfg::XLO_BUF_SIZE, len, sawEnd);
            tries++;
        }
        if (refused(xlo)) Con.printf("[dl] still refused after %d tries\n", tries);
        else Con.printf("[dl] log accepted after %lu s (%d tries)\n",
                        (unsigned long)((millis() - t0) / 1000), tries);
    }
    Con.printf("[dl] XLO: %u bytes, DATA END %s, reply %s\n", (unsigned)len,
                  sawEnd ? "yes" : "no", xlo.timeout ? "timeout" : xlo.code.c_str());

    // An incomplete dump is retried once. A flight is only saved with a complete log,
    // or with no log if the ECU does not support XLO (UC). Never save a partial log:
    // its key would make the real download look like a duplicate.
    // DATA START is checked too: on 5 Nov 2026 a retry sent mid-dump caught only the end of
    // the earlier dump (rows and DATA END but no header).
    auto complete = [&]() { return sawEnd && xlo.ok && strstr(xloBuf, "DATA START") != nullptr; };
    if (!complete() && xlo.code != "UC") {
        Con.println("[dl] dump incomplete - waiting for the ECU to go quiet, then retrying");
        ecu.drain(cfg::DRAIN_QUIET_MS);
        xlo = ecu.dumpLog(xloBuf, cfg::XLO_BUF_SIZE, len, sawEnd);
        Con.printf("[dl] retry XLO: %u bytes, DATA END %s, reply %s\n", (unsigned)len,
                      sawEnd ? "yes" : "no", xlo.timeout ? "timeout" : xlo.code.c_str());
        if (!complete()) {
            ecu.drain(cfg::DRAIN_QUIET_MS);
            g_lastResult = "log dump incomplete twice (" + String((unsigned)len) + " bytes) - nothing saved";
            return false;
        }
    }

    String logStatus = "none";
    XloResult parsed;
    if (complete()) {
        parsed = parseXlo(xloBuf, len);
        if (parsed.rowCount == 0 || parsed.badLines > 0) {
            g_lastResult = "log parse failed (" + String((unsigned)parsed.rowCount) + " rows, " +
                         String((unsigned)parsed.badLines) + " bad) - nothing saved";
            return false;
        }
        logStatus = "full";
    }
    Con.printf("[dl] parsed %u rows, %u bad, %u runs, wrap joined %s\n", (unsigned)parsed.rowCount,
                  (unsigned)parsed.badLines, (unsigned)parsed.runs.size(), parsed.wrapJoined ? "yes" : "no");

    // Same-session test: the ECU logs every second while running and cooling down, so
    // straight after a flight the last log row is within a few seconds of the ECU's
    // system time. A larger gap (or a system time below the last row) means the ECU
    // has been restarted since that flight, so its times and session counters
    // (RA1 pump min/max, RRC failsafes) do not belong to the logged flight.
    // Bench, 1 Nov 2026: system time 1003 s vs last row 725 s -> different session.
    RunSummary s;
    bool sameSession = false;
    long logGap = -1;
    if (!parsed.runs.empty()) {
        s = summariseRun(parsed.runs.back());
        logGap = sysTime - s.lastT;
        sameSession = (logGap >= 0 && logGap <= cfg::SAME_SESSION_MAX_GAP_S);
    }
    String timeSource;
    if (!clockValid()) timeSource = "unknown_no_clock";
    else if (parsed.runs.empty()) timeSource = "unknown_no_log";
    else if (sameSession) timeSource = "back_calculated";
    else if (logGap < 0) timeSource = "unknown_ecu_restarted";
    else timeSource = "unknown_log_older_than_session";
    auto stamp = [&](int32_t ecuT) -> String {
        if (timeSource != "back_calculated" || ecuT < 0) return "null";
        return jsonStr(isoTime(now - (sysTime - ecuT)));
    };

    String j = "{\n";
    j += "  \"key\": " + jsonStr(key) + ",\n";
    j += "  \"logger_fw\": " + jsonStr(FW) + ",\n";
    j += "  \"downloaded_at\": " + (clockValid() ? jsonStr(isoTime(now)) : String("null")) + ",\n";
    j += "  \"time_source\": " + jsonStr(timeSource) + ",\n";
    j += "  \"ecu_system_time_s\": " + String(sysTime) + ",\n";
    j += "  \"fw_type\": " + jsonStr(param(rty, 0)) + ",\n";
    j += "  \"fw_version\": " + jsonStr(param(rty, 1)) + ",\n";
    j += "  \"serial\": " + jsonStr(serial) + ",\n";
    j += "  \"turbine_type\": " + jsonStr(param(rty, 5)) + ",\n";
    j += "  \"runs_ok\": " + String(runsOk) + ",\n";
    j += "  \"starts_failed\": " + String(param(ri1, 1).toInt()) + ",\n";
    j += "  \"total_run_time_s\": " + String(param(ri1, 2).toFloat(), 1) + ",\n";
    j += "  \"last_run_time_s\": " + String(param(ri1, 3).toInt()) + ",\n";
    j += "  \"off_condition\": " + String(param(ra1, 0).toInt()) + ",\n";
    j += "  \"ambient_c\": " + String(param(ra1, 1).toFloat(), 1) + ",\n";
    // RA1 pump min/max and RRC failsafe counters reset at ECU power-up (seen 1 Nov 2026),
    // so they describe the flight only if the ECU has not been switched off since.
    j += "  \"session_values_valid\": " + String(sameSession ? "true" : "false") + ",\n";
    j += "  \"log_gap_s\": " + String(logGap) + ",\n";
    j += "  \"ecu_state_at_download\": " + (rac.timeout ? String("null") : String(param(rac, 3).toInt())) + ",\n";
    j += "  \"pump_v_min\": " + String(param(ra1, 2).toFloat(), 2) + ",\n";
    j += "  \"pump_v_max\": " + String(param(ra1, 3).toFloat(), 2) + ",\n";
    j += "  \"fuel_flow_ml_min\": " + String(param(rfi, 0).toInt()) + ",\n";
    j += "  \"fuel_remaining_ml\": " + String(param(rfi, 1).toInt()) + ",\n";
    j += "  \"battery_v\": " + String(param(rfi, 3).toFloat(), 2) + ",\n";
    j += "  \"failsafe_count\": " + String(param(rrc, 4).toInt()) + ",\n";
    j += "  \"failsafe_time_s\": " + String(param(rrc, 5).toFloat(), 1) + ",\n";
    j += "  \"log_status\": " + jsonStr(logStatus) + ",\n";
    j += "  \"log_rows\": " + String((unsigned)parsed.rowCount) + ",\n";
    j += "  \"log_runs\": " + String((unsigned)parsed.runs.size()) + ",\n";
    j += "  \"last_run\": {\n";
    j += "    \"first_t\": " + String(s.firstT) + ", \"last_t\": " + String(s.lastT) + ",\n";
    j += "    \"ignition_t\": " + String(s.ignitionT) + ", \"ignition_at\": " + stamp(s.ignitionT) + ",\n";
    j += "    \"run_t\": " + String(s.runT) + ",\n";
    j += "    \"shutdown_t\": " + String(s.shutdownT) + ", \"shutdown_at\": " + stamp(s.shutdownT) + ",\n";
    j += "    \"peak_start_egt_c\": " + String(s.peakStartEgt) + ", \"max_egt_c\": " + String(s.maxEgt) + ",\n";
    j += "    \"min_start_batt_v\": " + String(s.minStartBatt, 2) + ", \"max_pump_v\": " + String(s.maxPump, 2) + ",\n";
    j += "    \"min_flight_rpm\": " + String(s.minRunRpm) + ",\n";
    j += "    \"start_s\": " + String((s.ignitionT >= 0 && s.runT >= 0) ? s.runT - s.ignitionT : -1) + ",\n";
    j += "    \"flight_s\": " + String(s.flightSecs) + ",\n";
    j += "    \"mean_flight_rpm\": " + String(s.meanFlightRpm, 0) + ",\n";
    j += "    \"mean_flight_egt_c\": " + String(s.meanFlightEgt, 1) + ",\n";
    j += "    \"mean_flight_pump_v\": " + String(s.meanFlightPump, 3) + "\n  },\n";
    j += "  \"raw\": {\n";
    const Reply* all[] = {&rty, &ri1, &ra1, &rfi, &rrc, &rs1, &rac};
    const char* names[] = {"RTY", "RI1", "RA1", "RFI", "RRC", "RS1", "RAC"};
    for (int i = 0; i < 7; ++i) {
        j += "    \"" + String(names[i]) + "\": " + (all[i]->timeout ? String("null") : jsonStr(all[i]->raw));
        j += (i < 6) ? ",\n" : "\n";
    }
    j += "  }\n}\n";

    String base = "/flights/" + key;
    bool ok = true;
    if (len > 0) ok &= store::write(base + ".raw", (const uint8_t*)xloBuf, len);
    if (!parsed.runs.empty()) {
        std::string csv = runToCsv(parsed.runs.back());
        ok &= store::write(base + ".csv", (const uint8_t*)csv.data(), csv.size());
    }
    ok &= store::write(base + ".json", j);   // written last: its presence marks a complete flight
    if (!ok) { g_lastResult = "flash write failed for " + key; return false; }

    g_lastResult = "saved " + key + " (log " + logStatus + ", " + String((unsigned)parsed.rowCount) + " rows)";
    Con.println("[dl] " + g_lastResult);
    return true;
}

// ---------------------------------------------------------------- console
static void printHelp() {
    Con.println(
        "Commands:\n"
        "  status            state, last result, clock, flash use\n"
        "  ls                list stored flights\n"
        "  json <key>        print a flight's summary\n"
        "  csv <key>         print a flight's last run as CSV\n"
        "  raw <key>         print the raw XLO dump\n"
        "  rm <key>          delete one flight\n"
        "  rmall YES         delete all flights\n"
        "  time <epoch>      set the clock (UTC seconds), e.g. from https://www.epochconverter.com\n"
        "  dl                download again now (duplicates are still skipped)\n"
        "  wifi <ssid> <pw>  join home WiFi (for the browser and OTA updates); 'wifi off' to stop\n"
        "  net               WiFi, Bluetooth and firmware partition status\n"
        "  power             battery, wake reason, time left before sleep (battery build)\n"
        "  sleep             go to sleep now (battery build; wakes on ECU power or the button)\n"
        "  stay on|off       keep awake for bench work (battery build; until reboot)\n"
        "The same commands can be sent over WiFi or Bluetooth: Console tab on the web page,\n"
        "or POST /api/console?cmd=...; recent output is at GET /api/console.");
}

static void handleCommand(String line) {
    line.trim();
    if (!line.length()) return;
    int sp = line.indexOf(' ');
    String cmd = sp < 0 ? line : line.substring(0, sp);
    String arg = sp < 0 ? "" : line.substring(sp + 1);
    arg.trim();

    if (cmd == "help") printHelp();
    else if (cmd == "status") {
        const char* st[] = {"Idle", "Download", "Saved", "Fault"};
        Con.printf("state: %s\nlast: %s\nclock: %s\nflash: %u / %u KB\nflights: %u\n",
                      st[(int)state], g_lastResult.c_str(),
                      clockValid() ? isoTime(time(nullptr)).c_str() : "not set",
                      (unsigned)(store::usedBytes() / 1024), (unsigned)(store::totalBytes() / 1024),
                      (unsigned)store::listKeys().size());
    }
    else if (cmd == "ls") { for (auto& k : store::listKeys()) Con.println(k); }
    else if (cmd == "json" || cmd == "csv" || cmd == "raw") {
        if (!store::printFile("/flights/" + arg + "." + cmd, Con)) Con.println("not found");
        Con.println();
    }
    else if (cmd == "rm") Con.println(store::removeFlight(arg) ? "deleted" : "not found");
    else if (cmd == "rmall") {
        if (arg == "YES") { store::removeAll(); Con.println("all flights deleted"); }
        else Con.println("type: rmall YES");
    }
    else if (cmd == "time") {
        long e = arg.toInt();
        if (e < 1700000000) { Con.println("give UTC epoch seconds"); return; }
        setClock((time_t)e, "console");
    }
    else if (cmd == "dl") state = State::Download;
    else if (cmd == "wifi") {
        if (arg == "off" || !arg.length()) { net::setWifi("", ""); Con.println("WiFi off"); return; }
        // wifi MyNet password   or, for a name with spaces:   wifi "My Net" password
        String ssid, pass;
        if (arg.startsWith("\"") && arg.indexOf('"', 1) > 0) {
            int q = arg.indexOf('"', 1);
            ssid = arg.substring(1, q);
            pass = arg.substring(q + 1);
            pass.trim();
        } else {
            int sp2 = arg.indexOf(' ');
            ssid = sp2 < 0 ? arg : arg.substring(0, sp2);
            pass = sp2 < 0 ? String("") : arg.substring(sp2 + 1);
        }
        net::setWifi(ssid, pass);
        Con.println("saved - joining " + ssid);
    }
    else if (cmd == "power") {
        if (!cfg::HAS_BATTERY) { Con.println("power: USB test board, never sleeps"); return; }
        int mv = power::batteryMv();
        Con.printf("battery: %s%s\nwoke by: %s\nECU plugged in: %s\nsleep in: %u s%s\n",
                   mv > 0 ? (String(mv) + " mV").c_str() : "no reading (divider not fitted?)",
                   power::batteryLow() ? " - LOW" : "", power::wakeReason(), power::ecuPresent() ? "yes" : "no",
                   (unsigned)power::awakeSecondsLeft(), power::stayAwake() ? " (stay on)" : "");
    }
    else if (cmd == "sleep") power::sleepNow("console command");
    else if (cmd == "stay") {
        power::setStayAwake(arg != "off");
        Con.println(power::stayAwake() ? "staying awake until reboot or 'stay off'" : "normal sleep rules");
    }
    else if (cmd == "net") {
        Con.println("wifi: " + net::wifiStatus());
        Con.printf("bluetooth: advertising as %s\n", cfg::DEVICE_NAME);
    }
    else Con.println("unknown command - type help");
}

// Command as echoed into the console: a WiFi password is never shown.
static String echoText(const String& cmd) {
    if (cmd.startsWith("wifi ") && cmd != "wifi off") {
        String a = cmd.substring(5);
        a.trim();
        int cut = a.startsWith("\"") ? a.indexOf('"', 1) + 1 : a.indexOf(' ');
        if (cut > 0 && cut < (int)a.length()) return "> wifi " + a.substring(0, cut) + " ****";
    }
    return "> " + cmd;
}

static void pollConsole() {
    static String buf;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            buf.trim();
            if (buf.length()) Con.note(echoText(buf) + "\n");   // the monitor echoes it locally already
            handleCommand(buf);
            buf = "";
        }
        else if (buf.length() < 120) buf += c;
    }
    // One command from WiFi or Bluetooth per pass, run exactly like a typed one.
    String remote;
    if (console::takeCommand(remote)) {
        Con.println(echoText(remote) + "   (remote)");
        handleCommand(remote);
    }
}

// ---------------------------------------------------------------- main loop
void setup() {
#if ARDUINO_USB_CDC_ON_BOOT
    // XIAO: the console is the chip's own USB port. With a PC attached but no monitor open,
    // a full buffer would make each print wait up to 100 ms; a bigger buffer and a short
    // wait keep the ECU download unaffected. (With no USB host at all, prints never wait.)
    Serial.setTxBufferSize(4096);
    Serial.setTxTimeoutMs(10);
#endif
    Serial.begin(115200);
    Con.begin(cfg::CONSOLE_BUF_SIZE);
    power::begin();          // before the LEDs: releases pins held during deep sleep, powers the RTC
    leds.begin();
    showIdle();
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) delay(10);

    xloBuf = (char*)ps_malloc(cfg::XLO_BUF_SIZE);
    if (!xloBuf) xloBuf = (char*)malloc(cfg::XLO_BUF_SIZE);
    bool fsOk = store::begin();
    ecu.begin(ledsIdle);
    rtcclock::begin();
    net::begin();

    Con.printf("\nJetCat ECU Logger (fw %s). Flash %s, buffer %s. Type help.\n", FW,
                  fsOk ? "ok" : "FAILED", xloBuf ? "ok" : "FAILED");
    if (cfg::HAS_BATTERY) {
        int mv = power::batteryMv();
        Con.printf("[power] woke by %s, battery %s\n", power::wakeReason(),
                   mv > 0 ? (String(mv) + " mV").c_str() : "no reading (divider not fitted?)");
    }
    if (!fsOk || !xloBuf) { state = State::Fault; showFault(); }
}

void loop() {
    static uint32_t lastProbe = 0, lastReply = 0;
    leds.update();
    pollConsole();
    net::poll();
    power::poll();

    // A new firmware confirms itself after running 60 s without crashing (rollback guard).
    static bool fwConfirmed = false;
    if (!fwConfirmed && millis() > 60000) { net::markFirmwareGood(); fwConfirmed = true; }

    // Phone or browser asked for a download (only when the ECU is connected or idle).
    if (g_downloadRequested) { g_downloadRequested = false; if (!net::otaBusy()) state = State::Download; }

    switch (state) {
        case State::Idle:
            if (millis() - lastProbe >= cfg::PROBE_INTERVAL_MS) {
                lastProbe = millis();
                Reply r = ecu.query(Cmd::RTY);
                if (!r.timeout && r.ok) {
                    Con.println("[ecu] connected: " + r.raw);
                    state = State::Download;
                }
            }
            break;

        case State::Download: {
            if (net::otaBusy()) { state = State::Idle; break; }   // never talk to the ECU mid-update
            g_downloading = true;
            g_dumpBytes = 0;
            bool ok = doDownload();
            g_downloading = false;
            lastReply = millis();
            if (ok) { state = State::Saved; showSaved(); }
            else { Con.println("[dl] FAULT: " + g_lastResult); state = State::Fault; showFault(); }
            break;
        }

        case State::Saved:
        case State::Fault:
            // Stay here until the ECU stops answering (unplugged), then go back to Idle.
            if (millis() - lastProbe >= 3000) {
                lastProbe = millis();
                Reply r = ecu.query(Cmd::RTY);
                if (!r.timeout) lastReply = millis();
                if (millis() - lastReply > cfg::UNPLUG_TIMEOUT_MS) {
                    Con.println("[ecu] unplugged");
                    state = State::Idle;
                    showIdle();
                }
            }
            break;
    }
}
