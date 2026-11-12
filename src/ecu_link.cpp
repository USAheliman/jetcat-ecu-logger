#include "ecu_link.h"
#include "logger.h"
#include "console.h"

const char* cmdName(Cmd c) {
    switch (c) {
        case Cmd::RTY: return "RTY";  // firmware, run times, serial, turbine type
        case Cmd::RI1: return "RI1";  // runs OK, failed starts, times, system time
        case Cmd::RA1: return "RA1";  // off condition, ambient, pump V min/max
        case Cmd::RFI: return "RFI";  // fuel flow, fuel left, set RPM, battery, last run
        case Cmd::RRC: return "RRC";  // RC inputs, failsafe count and time
        case Cmd::RS1: return "RS1";  // second shaft / gearbox
        case Cmd::RAC: return "RAC";  // live RPM, EGT, pump, state, throttle
        case Cmd::XLO: return "XLO";  // log dump
    }
    return "RTY";
}

void EcuLink::begin(IdleHook hook) {
    hook_ = hook;
    port_.setRxBufferSize(4096);
    port_.begin(cfg::ECU_BAUD, SERIAL_8N1, cfg::PIN_ECU_RX, cfg::PIN_ECU_TX);
}

void EcuLink::send(Cmd c) {
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%u,%s,1\r", (unsigned)cfg::ECU_ADDR, cmdName(c));
    port_.write((const uint8_t*)buf, n);
    port_.flush();
}

void EcuLink::flushInput() {
    while (port_.available()) port_.read();
}

bool EcuLink::readLine(String& line, uint32_t deadline) {
    line = "";
    while ((int32_t)(deadline - millis()) > 0) {
        while (port_.available()) {
            char ch = (char)port_.read();
            if (ch == '\r' || ch == '\n') {
                if (line.length()) return true;
            } else if (line.length() < 200) {
                line += ch;
            }
        }
        idle();
        delay(2);
    }
    return false;
}

Reply EcuLink::parseHs(const String& line) {
    // "1,HS,OK,p1,p2,..."
    Reply r;
    r.timeout = false;
    r.raw = line;
    int start = 0, field = 0;
    for (int i = 0; i <= (int)line.length(); ++i) {
        if (i == (int)line.length() || line[i] == ',') {
            String f = line.substring(start, i);
            f.trim();
            if (field == 2) r.code = f;
            else if (field > 2) r.params.push_back(f);
            field++;
            start = i + 1;
        }
    }
    // The ECU sometimes ends a reply with a comma; drop that empty last field.
    if (!r.params.empty() && r.params.back().length() == 0 && line.endsWith(",")) r.params.pop_back();
    r.ok = (r.code == "OK");
    return r;
}

Reply EcuLink::query(Cmd c, uint32_t timeoutMs) {
    flushInput();
    send(c);
    String hsPrefix = String(cfg::ECU_ADDR) + ",HS,";
    uint32_t deadline = millis() + timeoutMs;
    String line;
    while (readLine(line, deadline)) {
        if (line.startsWith(hsPrefix)) return parseHs(line);
        // anything else is the echo of our command; ignore it
    }
    return Reply{};  // timeout
}

Reply EcuLink::dumpLog(char* buf, size_t cap, size_t& len, bool& sawEnd) {
    len = 0;
    sawEnd = false;
    buf[0] = '\0';
    flushInput();
    send(Cmd::XLO);

    String hsPrefix = String(cfg::ECU_ADDR) + ",HS,";
    uint32_t start = millis(), lastByte = millis();
    Reply hs;
    // The ECU pauses after the header before the first data row (over 3 s on 1 Nov,
    // over 5 s on 5 Nov 2026), so the long first-row limit applies until a data row
    // arrives. A data row starts with a digit AND contains a tab: the echo of our
    // command ("1,XLO,1") also starts with a digit but has no tab, and treating it as a
    // row made the short gap limit end the 5 Nov dump during that pause.
    bool gotRow = false;
    size_t lineStart = 0;

    while (millis() - start < cfg::XLO_TOTAL_TIMEOUT_MS) {
        uint32_t gapLimit = gotRow ? cfg::XLO_GAP_TIMEOUT_MS : cfg::XLO_FIRST_ROW_MS;
        if (millis() - lastByte >= gapLimit) break;
        bool done = false;
        while (port_.available()) {
            char ch = (char)port_.read();
            lastByte = millis();
            if (len < cap - 1) { buf[len++] = ch; buf[len] = '\0'; }
            g_dumpBytes = len;   // progress for the phone's status page
            if (ch == '\n') {
                if (!gotRow) {
                    size_t p = lineStart;
                    while (p < len && (buf[p] == ' ' || buf[p] == '\t' || buf[p] == '\r')) ++p;
                    bool digit = p < len && buf[p] >= '0' && buf[p] <= '9';
                    bool tab = memchr(buf + lineStart, '\t', len - lineStart) != nullptr;
                    if (digit && tab) {
                        gotRow = true;
                        Con.printf("[dl] first log row %.1f s after the request\n", (millis() - start) / 1000.0);
                    }
                }
                lineStart = len;
            }
            if (ch == '\r') {
                // An HS line ends with CR. Find the start of the last line.
                size_t s = len - 1;
                while (s > 0 && buf[s - 1] != '\n' && buf[s - 1] != '\r') --s;
                String last = String(buf + s).substring(0, len - 1 - s);
                if (last.startsWith(hsPrefix)) {
                    hs = parseHs(last);
                    done = true;
                    break;
                }
            }
        }
        if (done) break;
        idle();
        delay(2);
    }
    sawEnd = strstr(buf, "DATA END") != nullptr;
    return hs;
}

void EcuLink::drain(uint32_t quietMs, uint32_t maxMs) {
    uint32_t start = millis(), last = millis();
    while (millis() - last < quietMs && millis() - start < maxMs) {
        while (port_.available()) { port_.read(); last = millis(); }
        idle();
        delay(5);
    }
}
