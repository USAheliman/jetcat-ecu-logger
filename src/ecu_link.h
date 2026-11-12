#pragma once
// Read-only link to the JetCat ECU.
//
// SAFETY: the ONLY way this code can transmit to the ECU is send(Cmd), and Cmd lists
// read commands only. There is deliberately no function that sends free text, so no
// write (W..), turbine control (TCO) or keyboard (WKY/KEN) command can ever be sent.
// Protocol: JetCat "Serial interface of the JetCat ECU" v1.13, 26 Sep 2006.

#include <Arduino.h>
#include <vector>
#include "config.h"

enum class Cmd : uint8_t { RTY, RI1, RA1, RFI, RRC, RS1, RAC, XLO };
const char* cmdName(Cmd c);

struct Reply {
    bool   timeout = true;      // no HS line received
    bool   ok = false;          // return code "OK"
    String code;                // OK, UC, PA, NA, PR, PL, DF
    std::vector<String> params;
    String raw;                 // HS line exactly as received
};

class EcuLink {
public:
    // Called while waiting for the ECU so LEDs keep flashing.
    using IdleHook = void (*)();

    void begin(IdleHook hook);
    Reply query(Cmd c, uint32_t timeoutMs = cfg::REPLY_TIMEOUT_MS);

    // Sends XLO and stores everything received in buf (null-terminated).
    // sawEnd = "DATA END" received. Returns the trailing HS reply.
    Reply dumpLog(char* buf, size_t cap, size_t& len, bool& sawEnd);

    // Discard incoming bytes until the ECU has been quiet for quietMs (e.g. after an
    // interrupted dump, so the next command isn't mixed up with leftover log rows).
    void drain(uint32_t quietMs, uint32_t maxMs = 90000);

    // Parse an "1,HS,OK,p1,p2,..." reply line (also used to rebuild stored replies).
    static Reply parseHs(const String& line);

private:
    void send(Cmd c);            // the only transmit path
    bool readLine(String& line, uint32_t deadline);
    void flushInput();
    void idle() { if (hook_) hook_(); }

    HardwareSerial& port_ = Serial1;
    IdleHook hook_ = nullptr;
};
