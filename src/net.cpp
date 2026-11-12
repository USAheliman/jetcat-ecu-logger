// JetCat ECU Logger :: net.cpp - WiFi, Bluetooth, the API and firmware updates.
// The Bluetooth transport is adapted from Malcolm Messiter's LDRC RXV2 (GPL-3.0-or-later),
// see ble_bridge.h. The WiFi OTA upload follows the MCP fuel station's /ota approach.
#include "net.h"

#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

#include "ble_bridge.h"
#include "config.h"
#include "console.h"
#include "power.h"
#include "logger.h"
#include "store.h"

WebServer httpServer(80);

// Web page, built into the firmware (platformio.ini: board_build.embed_txtfiles), so a
// firmware update also updates the pages and LittleFS only ever holds flights.
extern const uint8_t index_html_start[] asm("_binary_data_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_data_index_html_end");

// Firmware rollback. If the bootloader was built with rollback support, a newly
// updated firmware boots "pending verify" and is confirmed by markFirmwareGood()
// after it has run for a while; if it crashes before that, the bootloader goes back
// to the previous firmware at the next reset. If the bootloader lacks rollback
// support these calls do nothing and USB flashing is the recovery path.
extern "C" bool verifyRollbackLater() { return true; }

namespace net {

static Preferences prefs;
static String wifiSsid, wifiPass;
static bool mdnsStarted = false;
static bool otaWifiActive = false;
static String otaWifiError;
static bool rebootPending = false;
static uint32_t rebootAtMs = 0;

static String jsonEsc(const String& s) {
    String o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((uint8_t)c >= 0x20) o += c;
    }
    return o;
}

static String isoNow() {
    time_t t = time(nullptr);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char b[24];
    strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return String(b);
}

static const char* otaStateName() {
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK) return "n/a";
    switch (st) {
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_VALID: return "valid";
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted";
        default: return "undefined";
    }
}

bool otaBusy() { return otaWifiActive || bleOtaActive; }
bool bleConnected() { return bleClientConnected; }

void markFirmwareGood() { esp_ota_mark_app_valid_cancel_rollback(); }

String ip() { return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String(""); }

String wifiStatus() {
    if (!wifiSsid.length()) return "off (no network saved - use: wifi <ssid> <password>)";
    if (WiFi.status() == WL_CONNECTED)
        return "connected to " + wifiSsid + ", http://" + ip() + "  or  http://" + cfg::MDNS_NAME + ".local" +
               "  (signal " + String(WiFi.RSSI()) + " dBm, channel " + String(WiFi.channel()) + ")";
    return "connecting to " + wifiSsid + "...";
}

// The web server may only start once WiFi has been switched on at least once: on the
// Arduino 2.0.x core, WebServer::begin() with WiFi never started crashes the TCP/IP stack
// (boot loop on a logger with no saved network). Once started it stays up through
// "wifi off" and network changes.
static bool httpStarted = false;
static void startHttpIfReady() {
    if (httpStarted || WiFi.getMode() == WIFI_OFF) return;
    httpServer.begin();
    httpStarted = true;
    Con.println("[http] web server started");
}

// Battery build: WiFi is switched off when the home network has not been seen for
// cfg::WIFI_TRY_MS (e.g. at the field), and stays off until the next wake.
static uint32_t wifiLastSeenMs = 0;
static bool wifiGaveUp = false;
static bool wifiJoined = false;   // joined at least once since this wake

static void startWifi() {
    wifiLastSeenMs = millis();
    wifiGaveUp = false;
    if (!wifiSsid.length()) { WiFi.mode(WIFI_OFF); return; }
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(cfg::MDNS_NAME);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    Con.println("[wifi] connecting to " + wifiSsid);
}

void setWifi(const String& ssid, const String& pass) {
    wifiSsid = ssid;
    wifiPass = pass;
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    WiFi.disconnect(true);
    mdnsStarted = false;
    startWifi();
    startHttpIfReady();
}

// ---------------------------------------------------------------- API handlers
static void sendJson(const String& j) { g_router.send(200, "application/json", j); }
static void sendErr(int code, const char* msg) {
    g_router.send(code, "application/json", String("{\"ok\":false,\"error\":\"") + msg + "\"}");
}

static void hIndex() {
    size_t len = index_html_end - index_html_start;
    if (len && index_html_start[len - 1] == 0) --len;   // embed_txtfiles adds a terminating NUL
    g_router.sendBytes(200, "text/html", index_html_start, len);
}

static void hStatus() {
    String j = "{";
    j += "\"fw\":\"" + jsonEsc(FW) + "\",";
    j += "\"state\":\"" + String(stateName()) + "\",";
    j += "\"downloading\":" + String(g_downloading ? "true" : "false") + ",";
    j += "\"dump_bytes\":" + String((unsigned)g_dumpBytes) + ",";
    j += "\"last\":\"" + jsonEsc(g_lastResult) + "\",";
    j += "\"clock\":" + (clockValid() ? "\"" + isoNow() + "\"" : String("null")) + ",";
    j += "\"rtc\":" + String(rtcPresent() ? "true" : "false") + ",";
    j += "\"flights\":" + String((unsigned)store::listKeys().size()) + ",";
    j += "\"flash_used_kb\":" + String((unsigned)(store::usedBytes() / 1024)) + ",";
    j += "\"flash_total_kb\":" + String((unsigned)(store::totalBytes() / 1024)) + ",";
    j += "\"wifi\":{\"ssid\":\"" + jsonEsc(wifiSsid) + "\",\"connected\":" +
         String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",\"ip\":\"" + ip() + "\",\"rssi\":" +
         String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + "},";
    j += "\"ble\":{\"on\":" + String(bleStarted ? "true" : "false") + ",\"client\":" +
         String(bleClientConnected ? "true" : "false") + "},";
    j += "\"via\":\"" + String(g_router.viaBle() ? "ble" : "wifi") + "\",";
    j += "\"ota_state\":\"" + String(otaStateName()) + "\",";
    j += "\"running_partition\":\"" + String(esp_ota_get_running_partition()->label) + "\",";
    j += "\"uptime_s\":" + String((unsigned long)(millis() / 1000)) + ",";
    j += "\"power\":{\"battery_mv\":" + String(power::batteryMv()) + ",\"low\":" + String(power::batteryLow() ? "true" : "false") +
         ",\"wake\":\"" + String(power::wakeReason()) + "\",\"awake_s_left\":" + String(power::awakeSecondsLeft()) +
         ",\"stay\":" + String(power::stayAwake() ? "true" : "false") + ",\"ecu_present\":" + String(power::ecuPresent() ? "true" : "false") + "}";
    sendJson(j + "}");
}

static void hFlights() {
    String j = "[";
    bool first = true;
    for (const String& k : store::listKeys()) {
        String f = store::readText("/flights/" + k + ".json");
        if (!f.length()) continue;
        if (!first) j += ",";
        j += f;
        first = false;
    }
    sendJson(j + "]");
}

static void hFlightJson() {
    String key = g_router.arg("key");
    if (!store::validKey(key)) { sendErr(400, "bad key"); return; }
    String f = store::readText("/flights/" + key + ".json");
    if (!f.length()) { sendErr(404, "no such flight"); return; }
    sendJson(f);
}

static void hFlightCsv() {
    String key = g_router.arg("key");
    if (!store::validKey(key)) { sendErr(400, "bad key"); return; }
    String f = store::readText("/flights/" + key + ".csv");
    if (!f.length()) { sendErr(404, "no log for this flight"); return; }
    g_router.sendHeader("Content-Disposition", "attachment; filename=\"" + key + ".csv\"");
    g_router.send(200, "text/csv", f);
}

static void hDelete() {
    String key = g_router.arg("key");
    if (!store::validKey(key)) { sendErr(400, "bad key"); return; }
    if (g_downloading) { sendErr(409, "busy downloading"); return; }
    sendJson(store::removeFlight(key) ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"not found\"}");
}

static void hHelis() { sendJson(store::helisJson()); }

static void hSetHeli() {
    bool ok = store::setHeliName(g_router.arg("serial"), g_router.arg("name"));
    if (!ok) { sendErr(400, "bad serial or name"); return; }
    sendJson(store::helisJson());
}

static void hTime() {
    long e = g_router.arg("epoch").toInt();
    if (e < 1700000000L) { sendErr(400, "bad epoch"); return; }
    // Only move the clock if it is unset or off by more than 2 s, so a page refresh
    // does not keep nudging a good RTC.
    long diff = (long)time(nullptr) - e;
    if (!clockValid() || diff > 2 || diff < -2) setClock((time_t)e, g_router.viaBle() ? "phone (Bluetooth)" : "browser (WiFi)");
    sendJson("{\"ok\":true,\"clock\":\"" + isoNow() + "\"}");
}

static void hDownload() {
    if (g_downloading) { sendErr(409, "already downloading"); return; }
    g_downloadRequested = true;
    sendJson("{\"ok\":true}");
}

static void hWifi() {
    String ssid = g_router.arg("ssid");
    if (ssid.length() > 32) { sendErr(400, "ssid too long"); return; }
    sendJson("{\"ok\":true}");
    setWifi(ssid, g_router.arg("pass"));
}

static void hReboot() {
    if (g_downloading) { sendErr(409, "busy downloading"); return; }
    sendJson("{\"ok\":true,\"rebooting\":true}");
    bleEarlyPump();
    rebootPending = true;
    rebootAtMs = millis() + 500;
}

// ---- firmware update over WiFi (PlatformIO upload_ota.py or the /ota page)
static const char OTA_PAGE[] =
    "<!doctype html><meta name=viewport content='width=device-width'><title>Logger update</title>"
    "<body style='font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px'>"
    "<h2>JetCat Logger firmware update</h2><p>Choose <b>firmware.bin</b> from "
    "<code>.pio/build/esp32s3_n16r8/</code>, then Upload.</p>"
    "<form method=POST action=/api/ota enctype=multipart/form-data>"
    "<input type=file name=firmware accept=.bin><p><button>Upload</button></p></form>"
    "<p><a href=/>Back to flights</a></p></body>";

static void hOtaPage() { httpServer.send(200, "text/html", OTA_PAGE); }

static void hOtaUpload() {
    HTTPUpload& u = httpServer.upload();
    if (u.status == UPLOAD_FILE_START) {
        otaWifiError = "";
        if (g_downloading) { otaWifiError = "busy downloading from the ECU"; return; }
        Con.printf("[ota] WiFi update: %s\n", u.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) otaWifiError = Update.errorString();
        else otaWifiActive = true;
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (otaWifiActive && Update.write(u.buf, u.currentSize) != u.currentSize) {
            otaWifiError = Update.errorString();
            Update.abort();
            otaWifiActive = false;
        }
    } else if (u.status == UPLOAD_FILE_END) {
        if (otaWifiActive) {
            if (!Update.end(true)) otaWifiError = Update.errorString();
            otaWifiActive = false;
            Con.printf("[ota] WiFi update %s (%u bytes)\n", otaWifiError.length() ? "FAILED" : "OK",
                          (unsigned)u.totalSize);
        }
    } else if (u.status == UPLOAD_FILE_ABORTED) {
        if (otaWifiActive) Update.abort();
        otaWifiActive = false;
        otaWifiError = "upload aborted";
    }
}

static void hOtaDone() {
    if (otaWifiError.length()) {
        httpServer.send(500, "application/json", "{\"ok\":false,\"error\":\"" + jsonEsc(otaWifiError) + "\"}");
        return;
    }
    httpServer.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    rebootPending = true;
    rebootAtMs = millis() + 800;
}

// ---- firmware update over Bluetooth (same routes as LDRC RXV2, so its app flow fits)
static void hBleOtaBegin() {
    if (g_downloading) { sendErr(409, "busy downloading from the ECU"); return; }
    if (g_router.arg("type").length() && g_router.arg("type") != "fw") { sendErr(400, "firmware only"); return; }
    uint32_t size = (uint32_t)g_router.arg("size").toInt();
    if (size == 0 || size > 3u * 1024u * 1024u) { sendErr(400, "bad size"); return; }
    if (bleOtaActive) {
        if (millis() - bleOtaLastChunkMs < 10000) { sendErr(409, "an update is being written"); return; }
        Update.abort();
        bleOtaActive = false;
    }
    if (!Update.begin(size, U_FLASH)) { sendErr(500, Update.errorString()); return; }
    bleOtaSize = size;
    bleOtaGot = 0;
    bleOtaError = "";
    bleOtaLastChunkMs = millis();
    bleOtaActive = true;
    Con.printf("[ota] Bluetooth update started, %u bytes\n", (unsigned)size);
    sendJson("{\"ok\":true}");
}

static void hBleOtaStatus() {
    char b[160];
    snprintf(b, sizeof(b), "{\"active\":%s,\"got\":%lu,\"size\":%lu,\"error\":\"%s\"}", bleOtaActive ? "true" : "false",
             (unsigned long)bleOtaGot, (unsigned long)bleOtaSize, jsonEsc(bleOtaError).c_str());
    sendJson(b);
}

static void hBleOtaEnd() {
    if (!bleOtaActive) { sendErr(409, "no transfer"); return; }
    bleOtaActive = false;
    bool ok = bleOtaGot == bleOtaSize && bleOtaError.length() == 0 && Update.end(true);
    String err = ok ? "" : (bleOtaError.length() ? bleOtaError : String(Update.errorString()));
    if (!ok) Update.abort();
    Con.printf("[ota] Bluetooth update %s\n", ok ? "written OK - reboot to run it" : "FAILED");
    if (ok) sendJson("{\"ok\":true}");
    else sendErr(500, err.c_str());
}

// ---------------------------------------------------------------- remote console
// GET /api/console?since=N  -> {"first":..,"next":..,"text":".."}; poll with since=next.
// Up to 4 KB per reply (keeps Bluetooth replies short); ask again while next < total.
static String jsonEscText(const String& s) {
    String o;
    o.reserve(s.length() + 32);
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", (unsigned)(uint8_t)c); o += b; }
        else o += c;
    }
    return o;
}

static void hConsole() {
    uint32_t since = g_router.hasArg("since") ? (uint32_t)strtoul(g_router.arg("since").c_str(), nullptr, 10) : 0;
    uint32_t next = 0, first = 0;
    String text = Con.read(since, 4096, &next, &first);
    uint32_t total = Con.total();
    String j = "{\"first\":" + String(first) + ",\"next\":" + String(next) + ",\"total\":" + String(total) +
               ",\"text\":\"" + jsonEscText(text) + "\"}";
    sendJson(j);
}

// POST /api/console?cmd=...  queues a command for the same handler as the serial console.
static void hConsoleCmd() {
    String cmd = g_router.arg("cmd");
    cmd.trim();
    if (!cmd.length()) { sendErr(400, "no command"); return; }
    if (cmd.length() > console::MAX_CMD_LEN) { sendErr(400, "command too long"); return; }
    if (!console::queueCommand(cmd)) { sendErr(503, "busy - try again"); return; }
    sendJson("{\"ok\":true}");
}

static void hNotFound() { g_router.send(404, "text/plain", "not found"); }

// ---------------------------------------------------------------- begin / poll
// Every request from a phone or browser restarts the battery build's 10-minute awake timer.
static void onRoute(const char* path, HTTPMethod m, WebServer::THandlerFunction fn) {
    g_router.on(path, m, [fn]() { power::noteActivity(); fn(); });
}

void begin() {
    prefs.begin("logger", false);
    wifiSsid = prefs.getString("ssid", "");
    wifiPass = prefs.getString("pass", "");

    onRoute("/", HTTP_GET, hIndex);
    onRoute("/index.html", HTTP_GET, hIndex);
    onRoute("/api/status.json", HTTP_GET, hStatus);
    onRoute("/api/flights.json", HTTP_GET, hFlights);
    onRoute("/api/flight.json", HTTP_GET, hFlightJson);
    onRoute("/api/flight.csv", HTTP_GET, hFlightCsv);
    onRoute("/api/delete", HTTP_POST, hDelete);
    onRoute("/api/helis.json", HTTP_GET, hHelis);
    onRoute("/api/heli", HTTP_POST, hSetHeli);
    onRoute("/api/time", HTTP_POST, hTime);
    onRoute("/api/download", HTTP_POST, hDownload);
    onRoute("/api/wifi", HTTP_POST, hWifi);
    onRoute("/api/reboot", HTTP_POST, hReboot);
    onRoute("/api/bleota/begin", HTTP_POST, hBleOtaBegin);
    onRoute("/api/bleota/status", HTTP_GET, hBleOtaStatus);
    onRoute("/api/bleota/end", HTTP_POST, hBleOtaEnd);
    onRoute("/api/bleota/reboot", HTTP_POST, hReboot);
    onRoute("/api/console", HTTP_GET, hConsole);
    onRoute("/api/console", HTTP_POST, hConsoleCmd);
    g_router.onNotFound(hNotFound);
    httpServer.on("/ota", HTTP_GET, hOtaPage);
    g_router.onUpload("/api/ota", hOtaDone, hOtaUpload);

    if (cfg::RADIOS_ALWAYS_ALLOWED) {
        startWifi();
        startHttpIfReady();
        bleStart(cfg::DEVICE_NAME);
    }
    Con.printf("[ota] running from %s, image state %s\n", esp_ota_get_running_partition()->label, otaStateName());
}

void poll() {
    static bool inPoll = false;   // poll() is also called while waiting on the ECU
    if (inPoll) return;
    inPoll = true;
    if (WiFi.status() == WL_CONNECTED && !mdnsStarted) {
        mdnsStarted = MDNS.begin(cfg::MDNS_NAME);
        if (mdnsStarted) MDNS.addService("http", "tcp", 80);
        Con.println("[wifi] " + wifiStatus());
    }
    // Battery build: if the home network is not found within WIFI_TRY_MS of waking, switch
    // WiFi off to save power. Once it has joined, a dropout (e.g. the router re-issuing
    // addresses, 11 Nov 2026) is ridden out - the ESP32 reconnects by itself - and with
    // "stay on" it never gives up.
    if (WiFi.status() == WL_CONNECTED) { wifiLastSeenMs = millis(); wifiJoined = true; }
    else if (cfg::HAS_BATTERY && !wifiGaveUp && !wifiJoined && !power::stayAwake() &&
             wifiSsid.length() && WiFi.getMode() != WIFI_OFF &&
             millis() - wifiLastSeenMs > cfg::WIFI_TRY_MS) {
        wifiGaveUp = true;
        wl_status_t st = WiFi.status();      // read before switching off
        const char* why = st == WL_NO_SSID_AVAIL ? "network not seen (antenna fitted? in range?)"
                        : st == WL_CONNECT_FAILED ? "connection refused (wrong password?)"
                        : "no answer within the time limit";
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        mdnsStarted = false;
        Con.printf("[wifi] could not join %s: %s - WiFi off until the next wake (Bluetooth stays on)\n", wifiSsid.c_str(), why);
    }
    httpServer.handleClient();
    blePoll();
    if (rebootPending && (int32_t)(millis() - rebootAtMs) > 0) {
        Con.println("[ota] rebooting");
        delay(100);
        ESP.restart();
    }
    inPoll = false;
}

}  // namespace net
