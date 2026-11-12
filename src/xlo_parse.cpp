#include "xlo_parse.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Split on tabs and spaces, dropping empty fields.
std::vector<std::string> splitFields(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '\t' || c == ' ') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool toInt(const std::string& s, int32_t& v) {
    char* end = nullptr;
    long x = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    v = (int32_t)x;
    return true;
}

bool toFloat(const std::string& s, float& v) {
    char* end = nullptr;
    float x = std::strtof(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return false;
    v = x;
    return true;
}

// Column order is taken from the ECU's own header row.
enum Col { C_TIME, C_RPM, C_SETRPM, C_TEMP, C_PUMP, C_STATE, C_THR, C_AUX, C_BATT, C_AIR, C_SETSPD, C_N };
const char* kColNames[C_N] = {"Time", "Rpm", "SetRpm", "Temp", "Pump", "State", "THR", "AUX", "Batt", "AirSpd", "SetSpd"};

}  // namespace

XloResult parseXlo(const char* data, size_t len) {
    XloResult res;
    int colIdx[C_N];
    size_t nCols = 0;

    std::vector<XloRow> rows;  // buffer order
    std::string line;
    bool inData = false;

    auto handleLine = [&](const std::string& raw) {
        // trim
        size_t a = raw.find_first_not_of(" \t");
        if (a == std::string::npos) return;
        std::string l = raw.substr(a);
        if (!inData) {
            if (l.compare(0, 5, "Time\t") == 0 || l.compare(0, 5, "Time ") == 0) {
                std::vector<std::string> h = splitFields(l);
                nCols = h.size();
                for (int c = 0; c < C_N; ++c) {
                    colIdx[c] = -1;
                    for (size_t i = 0; i < h.size(); ++i)
                        if (h[i] == kColNames[c]) colIdx[c] = (int)i;
                }
                res.foundHeader = true;
                inData = true;
            }
            return;
        }
        if (l.compare(0, 8, "DATA END") == 0) { res.foundEnd = true; inData = false; return; }

        std::vector<std::string> f = splitFields(l);
        if (f.size() != nCols) { res.badLines++; return; }
        XloRow r{};
        int32_t tmp;
        bool ok = true;
        auto geti = [&](Col c, int32_t& out, int32_t dflt) {
            if (colIdx[c] < 0) { out = dflt; return; }
            if (!toInt(f[colIdx[c]], out)) ok = false;
        };
        auto getf = [&](Col c, float& out) {
            if (colIdx[c] < 0) { out = 0; return; }
            if (!toFloat(f[colIdx[c]], out)) ok = false;
        };
        geti(C_TIME, r.time, -1);
        geti(C_RPM, r.rpm, 0);
        geti(C_SETRPM, r.setRpm, 0);
        geti(C_TEMP, r.egt, 0);
        getf(C_PUMP, r.pump);
        geti(C_STATE, tmp, 0); r.state = (int16_t)tmp;
        geti(C_THR, tmp, 0);   r.thr = (int16_t)tmp;
        geti(C_AUX, tmp, 0);   r.aux = (int16_t)tmp;
        getf(C_BATT, r.batt);
        geti(C_AIR, r.airSpd, 65535);
        geti(C_SETSPD, r.setSpd, 0);
        if (!ok || colIdx[C_TIME] < 0) { res.badLines++; return; }
        rows.push_back(r);
    };

    for (size_t i = 0; i < len; ++i) {
        char c = data[i];
        if (c == '\n' || c == '\r') {
            handleLine(line);
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    if (!line.empty()) handleLine(line);

    res.rowCount = rows.size();
    if (rows.empty()) return res;

    // Split into segments wherever Time goes backwards (new ECU power-on session).
    std::vector<XloRun> segs(1);
    segs[0].rows.push_back(rows[0]);
    for (size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].time < rows[i - 1].time) segs.emplace_back();
        segs.back().rows.push_back(rows[i]);
    }

    // Ring buffer wrap: the dump starts at buffer index 0, so the oldest part of the
    // session that wrapped appears at the END. If the last segment's time runs straight
    // into the first segment's (0-2 s gap), join it on the front.
    // Observed 1 Nov 2026: [A 294..635][B 67..725][A 293] -> A = 293..635, B = 67..725.
    if (segs.size() > 1) {
        int32_t gap = segs.front().rows.front().time - segs.back().rows.back().time;
        if (gap >= 0 && gap <= 2) {
            std::vector<XloRow> joined = segs.back().rows;
            joined.insert(joined.end(), segs.front().rows.begin(), segs.front().rows.end());
            segs.front().rows.swap(joined);
            segs.pop_back();
            res.wrapJoined = true;
        }
    }
    // Segments are now oldest first; the most recent run is the last one.
    res.runs = segs;
    return res;
}

RunSummary summariseRun(const XloRun& run) {
    RunSummary s;
    if (run.rows.empty()) return s;
    s.firstT = run.rows.front().time;
    s.lastT = run.rows.back().time;
    for (const auto& r : run.rows) {
        if (s.ignitionT < 0 && r.state == 2) s.ignitionT = r.time;
        if (s.runT < 0 && r.state == 11) s.runT = r.time;
        if (s.runT >= 0 && s.shutdownT < 0 && r.state == 8) s.shutdownT = r.time;
        if (r.pump > s.maxPump) s.maxPump = r.pump;
    }
    bool battSet = false;
    double sumRpm = 0, sumEgt = 0, sumPump = 0;
    long nFlight = 0;
    for (const auto& r : run.rows) {
        bool beforeRun = (s.runT < 0) || (r.time < s.runT);
        if (beforeRun) {
            if (r.egt > s.peakStartEgt) s.peakStartEgt = r.egt;
            if (!battSet || r.batt < s.minStartBatt) { s.minStartBatt = r.batt; battSet = true; }
        }
        if (r.state == 11 && r.egt > s.maxEgt) s.maxEgt = r.egt;
        if (r.state == 11 && r.thr >= 90) s.flightSecs++;
        if (r.state == 11 && r.thr >= 90 && s.runT >= 0 && r.time > s.runT + 30) {
            if (s.minRunRpm < 0 || r.rpm < s.minRunRpm) s.minRunRpm = r.rpm;
            sumRpm += r.rpm; sumEgt += r.egt; sumPump += r.pump; ++nFlight;
        }
    }
    if (nFlight) {
        s.meanFlightRpm = (float)(sumRpm / nFlight);
        s.meanFlightEgt = (float)(sumEgt / nFlight);
        s.meanFlightPump = (float)(sumPump / nFlight);
    }
    return s;
}

std::string runToCsv(const XloRun& run) {
    std::string out = "time_s,rpm,set_rpm,egt_c,pump_v,state,throttle_pct,aux,batt_v,airspd,set_spd\n";
    out.reserve(run.rows.size() * 48 + 80);
    char buf[128];
    for (const auto& r : run.rows) {
        char air[12] = "";
        if (r.airSpd != 65535) std::snprintf(air, sizeof(air), "%ld", (long)r.airSpd);
        std::snprintf(buf, sizeof(buf), "%ld,%ld,%ld,%ld,%.2f,%d,%d,%d,%.2f,%s,%ld\n",
                      (long)r.time, (long)r.rpm, (long)r.setRpm, (long)r.egt, r.pump,
                      r.state, r.thr, r.aux, r.batt, air, (long)r.setSpd);
        out += buf;
    }
    return out;
}
