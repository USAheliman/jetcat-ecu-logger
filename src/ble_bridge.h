// JetCat ECU Logger :: ble_bridge.h
//
// HTTP-over-Bluetooth bridge, adapted from Malcolm Messiter's LockDownRadioControl
// RXV2 receiver (RXV2/src/BleConfig.h, https://github.com/Mmessiter/LDRC_V2),
// used with his permission under GPL-3.0-or-later.
// Copyright (C) 2026 Malcolm Messiter (original bridge); adaptation for the logger 2026.
//
// One set of request handlers serves two transports:
//   WiFi       -> the Arduino WebServer (httpServer) as normal
//   Bluetooth  -> requests arrive framed on a GATT characteristic, run through the
//                 SAME handlers, and the captured reply is streamed back as notifies.
//
// Framing (unchanged from RXV2, so Malcolm's app shells speak it as-is):
//   App -> logger (REQ, write):  'Q' <totalLen> '|' <payload>   then '+' <payload> ...
//       payload = METHOD SP path[?query] '\n' {Header: value '\n'} '\n' body
//   Logger -> app (RESP, notify): 'R' code '|' contentType '|' bodyLen '|' location '\n'
//       then raw body bytes in chunks of at most 240 bytes.
//   Raw OTA chunk (REQ, write without response): 0xA5 <u32 offset LE> <payload>
//
// Requests are only queued by the NimBLE task; they are executed from loop() by
// blePoll(), the same task as the WiFi server, so handlers never race each other.
//
// Changes from RXV2: no channel streaming, no receiver flight rules or link stats,
// events go to the console (Con), the router forwards to `httpServer`, firmware-only BLE OTA
// (the logger's LittleFS holds flights, never web pages).

#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <NimBLEDevice.h>
#include <Update.h>
#include <map>
#include <vector>
#include "console.h"

extern WebServer httpServer;

// Our own service (new UUIDs, so the logger is never mistaken for an LDRC receiver).
static const char* BLE_SVC_UUID  = "4a430001-7c2e-4f1b-9d6a-1b05e1c0f3a1";
static const char* BLE_REQ_UUID  = "4a430002-7c2e-4f1b-9d6a-1b05e1c0f3a1";
static const char* BLE_RESP_UUID = "4a430003-7c2e-4f1b-9d6a-1b05e1c0f3a1";

constexpr size_t   BLE_MAX_REQUEST  = 16 * 1024;
constexpr size_t   BLE_MAX_RESPONSE = 200 * 1024;   // flight list + one flight CSV fit easily
constexpr uint32_t BLE_PUMP_US      = 3000;         // at most ~3 ms of loop() per pass
constexpr size_t   BLE_PUMP_BUDGET  = 6 * 1024;

// ---- state (NimBLE task writes, loop() executes) ----
inline volatile bool bleClientConnected = false;
inline bool          bleStarted = false;
inline bool          bleInited  = false;
inline uint32_t      bleLastActivityMs = 0;
inline uint16_t      bleConnMtu = 23;

inline String        bleInbox;
inline size_t        bleInboxExpected = 0;
inline volatile bool bleReqReady = false;
inline volatile bool bleReplyStale = false;

inline volatile bool     bleOtaActive = false;
inline volatile uint32_t bleOtaSize = 0;
inline volatile uint32_t bleOtaGot = 0;
inline volatile uint32_t bleOtaLastChunkMs = 0;
inline String            bleOtaError;

inline bool   bleActive = false;   // a handler is running for a BLE request
inline bool   bleSent = false;
inline int    bleCode = 200;
inline String bleType, bleBody, bleLocation, bleReqId;
inline volatile bool blePumping = false;
inline String bleHeaderFrame;
inline size_t bleTxOffset = 0;
inline NimBLECharacteristic* bleRespChr = nullptr;

inline String bleMethod, blePath;
inline std::map<String, String> bleArgs;
inline std::map<String, String> bleHeaders;
inline String bleRawBody;

inline String bleUrlDecode(const String& s) {
    String out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '+') out += ' ';
        else if (c == '%' && i + 2 < s.length()) {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return 0;
            };
            out += (char)((hex(s[i + 1]) << 4) | hex(s[i + 2]));
            i += 2;
        } else out += c;
    }
    return out;
}

inline void bleParseParams(const String& s) {
    size_t start = 0;
    while (start < s.length()) {
        int amp = s.indexOf('&', start);
        String pair = (amp < 0) ? s.substring(start) : s.substring(start, amp);
        int eq = pair.indexOf('=');
        if (eq > 0) bleArgs[bleUrlDecode(pair.substring(0, eq))] = bleUrlDecode(pair.substring(eq + 1));
        else if (pair.length()) bleArgs[bleUrlDecode(pair)] = "";
        if (amp < 0) break;
        start = amp + 1;
    }
}

// ---------------------------------------------------------------- ReqRouter
// WebServer look-alike: register handlers once; they answer WiFi or BLE.
struct BleRoute {
    String path;
    HTTPMethod method;
    WebServer::THandlerFunction fn;
};

class ReqRouter {
public:
    std::vector<BleRoute> routes;
    WebServer::THandlerFunction notFoundFn;

    void on(const String& path, HTTPMethod m, WebServer::THandlerFunction fn) {
        routes.push_back({path, m, fn});
        httpServer.on(path, m, fn);
    }
    // WiFi-only upload route (multipart firmware upload from PlatformIO or a browser).
    void onUpload(const String& path, WebServer::THandlerFunction done, WebServer::THandlerFunction upload) {
        httpServer.on(path, HTTP_POST, done, upload);
    }
    void onNotFound(WebServer::THandlerFunction fn) {
        notFoundFn = fn;
        httpServer.onNotFound(fn);
    }

    bool viaBle() const { return bleActive; }
    String arg(const String& name) {
        if (bleActive) { auto it = bleArgs.find(name); return it == bleArgs.end() ? String() : it->second; }
        return httpServer.arg(name);
    }
    bool hasArg(const String& name) {
        if (bleActive) return bleArgs.count(name) > 0;
        return httpServer.hasArg(name);
    }
    String body() {   // raw request body (JSON posts)
        if (bleActive) return bleRawBody;
        return httpServer.arg("plain");
    }
    String uri() { return bleActive ? blePath : httpServer.uri(); }

    void sendHeader(const String& k, const String& v) {
        if (bleActive) { if (k.equalsIgnoreCase("Location")) bleLocation = v; return; }
        httpServer.sendHeader(k, v);
    }
    void send(int code, const char* type, const String& body) {
        if (bleActive) {
            if (body.length() > BLE_MAX_RESPONSE) {
                bleCode = 413; bleType = "text/plain"; bleBody = "too large for Bluetooth - use WiFi"; bleSent = true;
                return;
            }
            bleCode = code; bleType = type; bleBody = body; bleSent = true;
            return;
        }
        httpServer.send(code, type, body);
    }
    void send(int code, const char* type, const char* body) { send(code, type, String(body)); }
    void sendBytes(int code, const char* type, const uint8_t* data, size_t len) {
        if (bleActive) {
            if (len > BLE_MAX_RESPONSE) { send(413, "text/plain", "too large for Bluetooth - use WiFi"); return; }
            bleCode = code; bleType = type; bleBody = ""; bleBody.concat((const char*)data, len); bleSent = true;
            return;
        }
        httpServer.send_P(code, type, (const char*)data, len);
    }
};

inline ReqRouter g_router;

// ---------------------------------------------------------------- NimBLE callbacks
class BleReqCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
        bleLastActivityMs = millis();
        std::string v = c->getValue();
        if (v.empty()) return;
        if (bleReqReady) {                    // previous request still running
            if (v[0] == 'Q') bleReplyStale = true;
            return;
        }
        if (v[0] == 'Q') {
            if (blePumping) blePumping = false;      // app gave up on the last reply
            size_t bar = v.find('|');
            if (bar == std::string::npos) return;
            bleInboxExpected = strtoul(v.substr(1, bar - 1).c_str(), nullptr, 10);
            if (bleInboxExpected == 0 || bleInboxExpected > BLE_MAX_REQUEST) { bleInboxExpected = 0; return; }
            bleInbox = "";
            bleInbox.reserve(bleInboxExpected);
            bleInbox.concat(v.data() + bar + 1, v.size() - bar - 1);
        } else if (v[0] == '+') {
            if (!bleInboxExpected) return;
            bleInbox.concat(v.data() + 1, v.size() - 1);
        } else if ((uint8_t)v[0] == 0xA5) {          // raw OTA chunk
            if (!bleOtaActive || v.size() < 6) return;
            uint32_t off = (uint8_t)v[1] | ((uint8_t)v[2] << 8) | ((uint8_t)v[3] << 16) | ((uint32_t)(uint8_t)v[4] << 24);
            if (off != bleOtaGot) return;            // out of sequence: the app resyncs via status
            size_t n = v.size() - 5;
            if (Update.write((uint8_t*)v.data() + 5, n) == n) bleOtaGot += n;
            else if (bleOtaError.length() == 0) bleOtaError = Update.errorString();
            bleOtaLastChunkMs = millis();
            return;
        } else return;
        if (bleInboxExpected && bleInbox.length() >= bleInboxExpected) {
            bleReplyStale = false;
            bleReqReady = true;
        }
    }
};

class BleServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
        bleClientConnected = true;
        bleLastActivityMs = millis();
        bleConnMtu = 23;
        Con.println("[ble] app connected");
        s->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
    }
    void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override { bleConnMtu = mtu; }
    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
        bleClientConnected = false;
        bleConnMtu = 23;
        bleReqReady = false;
        blePumping = false;
        Con.printf("[ble] app disconnected (reason 0x%02x)\n", (unsigned)(reason & 0xFF));
        if (bleStarted) NimBLEDevice::startAdvertising();
    }
};

// ---------------------------------------------------------------- start / stop
inline void bleInitOnce(const char* name) {
    if (bleInited) return;
    NimBLEDevice::init(name);
    NimBLEDevice::setMTU(517);
    NimBLEServer* srv = NimBLEDevice::createServer();
    srv->setCallbacks(new BleServerCallbacks());
    NimBLEService* svc = srv->createService(BLE_SVC_UUID);
    NimBLECharacteristic* req = svc->createCharacteristic(BLE_REQ_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    req->setCallbacks(new BleReqCallbacks());
    bleRespChr = svc->createCharacteristic(BLE_RESP_UUID, NIMBLE_PROPERTY::NOTIFY);
    // svc->start() not needed: NimBLE-Arduino 2.x starts services with the server.
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SVC_UUID);
    adv->enableScanResponse(true);   // must come before setName (see RXV2 BleConfig.h)
    adv->setName(name);
    bleInited = true;
}

inline void bleStart(const char* name) {
    if (bleStarted) return;
    bleInitOnce(name);
    NimBLEDevice::startAdvertising();
    bleStarted = true;
    Con.printf("[ble] advertising as '%s'\n", name);
}

inline void bleStop() {
    if (!bleStarted) return;
    bleStarted = false;
    NimBLEDevice::stopAdvertising();
    NimBLEServer* srv = NimBLEDevice::getServer();
    if (srv) {
        for (int i = 0; srv->getConnectedCount() > 0 && i < 50; i++) {
            srv->disconnect(srv->getPeerInfo(0));
            delay(10);
        }
    }
    bleClientConnected = false;
    bleReqReady = false;
    blePumping = false;
    Con.println("[ble] off");
}

// Push a reply immediately from inside a handler (for replies followed by a restart).
inline bool bleEarlyPumped = false;
inline void bleEarlyPump() {
    if (!bleActive || !bleSent || !bleClientConnected || !bleRespChr) return;
    uint32_t t0 = millis();
    auto over = [&]() { return millis() - t0 >= 300; };
    String loc = (bleLocation.length() || !bleReqId.length()) ? bleLocation : ("#" + bleReqId);
    String hdr = "R" + String(bleCode) + "|" + bleType + "|" + String(bleBody.length()) + "|" + loc + "\n";
    bool headerOut = false;
    while (!(headerOut = bleRespChr->notify((const uint8_t*)hdr.c_str(), hdr.length())) && !over()) delay(5);
    size_t chunk = bleConnMtu > 23 ? bleConnMtu - 3 : 20;
    if (chunk > 240) chunk = 240;
    size_t off = 0;
    while (headerOut && off < bleBody.length()) {
        size_t n = min(chunk, bleBody.length() - off);
        if (!bleRespChr->notify((const uint8_t*)bleBody.c_str() + off, n)) { if (over()) break; delay(5); continue; }
        off += n;
    }
    if (headerOut) delay(100);
    bleBody = "";
    bleEarlyPumped = true;
}

// ---------------------------------------------------------------- execute + pump
inline void bleExecuteRequest() {
    bleMethod = ""; blePath = ""; bleArgs.clear(); bleHeaders.clear(); bleRawBody = "";
    int nl = bleInbox.indexOf('\n');
    if (nl < 0) { bleReqReady = false; return; }
    String reqLine = bleInbox.substring(0, nl);
    int sp = reqLine.indexOf(' ');
    if (sp < 0) { bleReqReady = false; return; }
    bleMethod = reqLine.substring(0, sp);
    String pq = reqLine.substring(sp + 1);
    int qm = pq.indexOf('?');
    blePath = (qm < 0) ? pq : pq.substring(0, qm);
    if (qm >= 0) bleParseParams(pq.substring(qm + 1));

    int pos = nl + 1;
    while (pos < (int)bleInbox.length()) {
        int le = bleInbox.indexOf('\n', pos);
        if (le < 0) { pos = bleInbox.length(); break; }
        String line = bleInbox.substring(pos, le);
        pos = le + 1;
        if (line.length() == 0) break;
        int colon = line.indexOf(':');
        if (colon > 0) {
            String v = line.substring(colon + 1);
            v.trim();
            bleHeaders[line.substring(0, colon)] = v;
        }
    }
    bleRawBody = bleInbox.substring(pos);
    if (bleMethod == "POST" && bleRawBody.length() && bleRawBody[0] != '{' && bleRawBody[0] != '[')
        bleParseParams(bleRawBody);                     // form-encoded
    bleReqId = "";
    { auto it = bleHeaders.find("X-Req"); if (it != bleHeaders.end()) bleReqId = it->second; }

    HTTPMethod m = (bleMethod == "POST") ? HTTP_POST : HTTP_GET;
    WebServer::THandlerFunction fn = g_router.notFoundFn;
    for (auto& r : g_router.routes)
        if (r.path == blePath && (r.method == HTTP_ANY || r.method == m)) { fn = r.fn; break; }

    bleActive = true;
    bleSent = false;
    bleCode = 404; bleType = "text/plain"; bleBody = "not found"; bleLocation = "";
    if (fn) fn();
    bleActive = false;

    if (bleEarlyPumped) {
        bleEarlyPumped = false;
        bleBody = ""; bleHeaderFrame = ""; blePumping = false;
        bleInbox = ""; bleInboxExpected = 0; bleReqReady = false;
        return;
    }
    if (!bleSent) { bleCode = 500; bleType = "text/plain"; bleBody = "handler sent nothing"; }
    if (bleReplyStale) {
        bleReplyStale = false;
        bleBody = ""; bleHeaderFrame = "";
        bleInbox = ""; bleInboxExpected = 0; bleReqReady = false;
        return;
    }
    String loc = (bleLocation.length() || !bleReqId.length()) ? bleLocation : ("#" + bleReqId);
    bleHeaderFrame = "R" + String(bleCode) + "|" + bleType + "|" + String(bleBody.length()) + "|" + loc + "\n";
    bleTxOffset = 0;
    blePumping = true;
    bleInbox = ""; bleInboxExpected = 0; bleReqReady = false;
}

inline void blePoll() {
    // Abandon a Bluetooth firmware transfer whose chunks stop: nothing is committed
    // until Update.end(), so the running firmware stays bootable.
    if (bleOtaActive && millis() - bleOtaLastChunkMs > 30000) {
        Update.abort();
        bleOtaActive = false;
        bleOtaError = "transfer stalled";
        Con.println("[ota] Bluetooth transfer stalled - aborted, old firmware intact");
    }
    if (!bleStarted && !bleClientConnected) return;
    if (bleReqReady && !blePumping) bleExecuteRequest();
    if (!blePumping || !bleRespChr || !bleClientConnected) {
        if (blePumping && !bleClientConnected) blePumping = false;
        return;
    }
    // Chunks capped at 240 bytes: some phones advertise a big MTU but drop larger
    // notifies (lesson from RXV2 and the MCP fuel station bridge).
    size_t mtu = bleConnMtu > 23 ? bleConnMtu : NimBLEDevice::getMTU();
    size_t chunk = mtu > 23 ? mtu - 3 : 20;
    if (chunk > 240) chunk = 240;
    size_t sent = 0;
    if (bleTxOffset == 0 && bleHeaderFrame.length()) {
        if (!bleRespChr->notify((const uint8_t*)bleHeaderFrame.c_str(), bleHeaderFrame.length())) return;
        sent += bleHeaderFrame.length();
        bleHeaderFrame = "";
    }
    uint32_t t0 = micros();
    while (bleTxOffset < bleBody.length() && sent < BLE_PUMP_BUDGET) {
        if (!blePumping) return;
        if (micros() - t0 > BLE_PUMP_US) return;
        size_t n = min(chunk, bleBody.length() - bleTxOffset);
        if (!bleRespChr->notify((const uint8_t*)bleBody.c_str() + bleTxOffset, n)) return;
        bleTxOffset += n;
        sent += n;
    }
    if (bleTxOffset >= bleBody.length() && bleHeaderFrame.length() == 0) {
        blePumping = false;
        bleBody = "";
    }
}
