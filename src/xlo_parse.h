#pragma once
// Pure C++ parser for the JetCat "XLO" log dump. No Arduino dependencies,
// so it is unit-tested on the PC against a real dump (test/test_parse).
//
// Dump format (measured on ECU V6, firmware 6.42, 1 Nov 2026):
//   echo "1,XLO,1\r", "DATA START\n", header "Time\tRpm\t...\t\n",
//   1,000 rows of 11 tab-separated fields ending "\t\n", "DATA END\n", "1,HS,OK\r".
// Rows come in ring-buffer order, NOT time order. Time = seconds since ECU power-on.

#include <stdint.h>
#include <string>
#include <vector>

struct XloRow {
    int32_t time;      // s since ECU power-on
    int32_t rpm;
    int32_t setRpm;
    int32_t egt;       // deg C
    float   pump;      // V
    int16_t state;     // JetCat Table 3
    int16_t thr;       // %
    int16_t aux;
    float   batt;      // V
    int32_t airSpd;    // 65535 = no sensor
    int32_t setSpd;
};

// One continuous stretch of log (one ECU power-on session), oldest row first.
struct XloRun {
    std::vector<XloRow> rows;
};

struct XloResult {
    bool   foundHeader = false;
    bool   foundEnd    = false;
    size_t rowCount    = 0;     // rows parsed
    size_t badLines    = 0;     // lines that could not be parsed
    bool   wrapJoined  = false; // ring-buffer tail rejoined to its run
    std::vector<XloRun> runs;   // oldest first; runs.back() = most recent
};

// Parse a raw dump (may include the echo and the trailing HS line).
XloResult parseXlo(const char* data, size_t len);

// Key points of one run, all in ECU seconds (-1 if not present).
struct RunSummary {
    int32_t firstT = -1, lastT = -1;
    int32_t ignitionT = -1;   // first state 2 (Ignite)
    int32_t runT = -1;        // first state 11 (Run)
    int32_t shutdownT = -1;   // first state 8 (Slow down) after Run
    int32_t peakStartEgt = -1;// max EGT before Run
    int32_t maxEgt = -1;      // max EGT in Run
    float   minStartBatt = 0; // min battery before Run
    float   maxPump = 0;
    int32_t minRunRpm = -1;   // min RPM at >= 90 % throttle in Run, after 30 s settle
    // Means over "flight" rows: state 11, throttle >= 90 %, more than 30 s after Run.
    int32_t flightSecs = 0;   // rows counted at flight throttle (all of Run with THR >= 90)
    float   meanFlightRpm = 0;
    float   meanFlightEgt = 0;
    float   meanFlightPump = 0;   // the pump-wear trend metric
};
RunSummary summariseRun(const XloRun& run);

// CSV of a run, oldest first, header row included. AirSpd 65535 -> empty.
std::string runToCsv(const XloRun& run);
