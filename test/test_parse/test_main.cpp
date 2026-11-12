// Host test: pio test -e native
// Parses the real dump captured from the BO 105 ECU (serial 3934) on 1 Nov 2026
// and checks the values against the Excel workbook made from the same dump.
#include <unity.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include "../../src/xlo_parse.h"

static std::string loadDump() {
    std::ifstream f("test/data/xlo_2026-11-01.log", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void setUp() {}
void tearDown() {}

void test_parses_all_rows() {
    std::string d = loadDump();
    TEST_ASSERT_TRUE_MESSAGE(d.size() > 40000, "test dump not found - run from project folder");
    XloResult r = parseXlo(d.data(), d.size());
    TEST_ASSERT_TRUE(r.foundHeader);
    TEST_ASSERT_TRUE(r.foundEnd);
    TEST_ASSERT_EQUAL_UINT32(1000, r.rowCount);
    TEST_ASSERT_EQUAL_UINT32(0, r.badLines);
}

void test_splits_runs_and_rejoins_wrap() {
    std::string d = loadDump();
    XloResult r = parseXlo(d.data(), d.size());
    TEST_ASSERT_TRUE(r.wrapJoined);
    TEST_ASSERT_EQUAL_UINT32(2, r.runs.size());
    TEST_ASSERT_EQUAL_INT32(293, r.runs[0].rows.front().time);
    TEST_ASSERT_EQUAL_INT32(635, r.runs[0].rows.back().time);
    TEST_ASSERT_EQUAL_UINT32(343, r.runs[0].rows.size());
    TEST_ASSERT_EQUAL_INT32(67, r.runs[1].rows.front().time);
    TEST_ASSERT_EQUAL_INT32(725, r.runs[1].rows.back().time);
    TEST_ASSERT_EQUAL_UINT32(657, r.runs[1].rows.size());
}

void test_last_flight_summary_matches_workbook() {
    std::string d = loadDump();
    XloResult r = parseXlo(d.data(), d.size());
    RunSummary s = summariseRun(r.runs.back());
    TEST_ASSERT_EQUAL_INT32(71, s.ignitionT);
    TEST_ASSERT_EQUAL_INT32(108, s.runT);
    TEST_ASSERT_EQUAL_INT32(706, s.shutdownT);
    TEST_ASSERT_EQUAL_INT32(722, s.peakStartEgt);
    TEST_ASSERT_EQUAL_INT32(478, s.maxEgt);
    TEST_ASSERT_EQUAL_INT32(86650, s.minRunRpm);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.44f, s.minStartBatt);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.62f, s.maxPump);
    // Workbook Summary sheet: 562 s at flight throttle, mean RPM 93,543.8,
    // mean EGT 412.80 C, mean pump 1.3787 V (first 30 s after Run excluded)
    TEST_ASSERT_EQUAL_INT32(562, s.flightSecs);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 93543.8f, s.meanFlightRpm);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 412.80f, s.meanFlightEgt);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 1.3787f, s.meanFlightPump);
}

void test_aux_and_batt_are_separate_fields() {
    std::string d = loadDump();
    XloResult r = parseXlo(d.data(), d.size());
    const XloRow& first = r.runs.back().rows.front();  // "67 1150 3000 63 0.47 16 19 0 7.64 0 0"
    TEST_ASSERT_EQUAL_INT16(0, first.aux);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.64f, first.batt);
}


// Ring-buffer layout of the V6 dump of 10 Nov 2026 (flight 3934-0557):
// [X 498..635][A 67..725][B 22..135][C 24..115][X 497]. The wrap row 497 is later than
// C's last row, so before the fix it was glued onto C and C was moved to the front.
static void addRows(std::string& d, int from, int to) {
    char b[96];
    for (int t = from; t <= to; ++t) {
        std::snprintf(b, sizeof(b), "%d\t33000\t33000\t400\t0.36\t11\t19\t0\t7.80\t0\t0\t\n", t);
        d += b;
    }
}
void test_wrap_row_later_than_newest_run() {
    std::string d = "1,XLO,1\rDATA START\nTime\tRpm\tSetRpm\tTemp\tPump\tState\tTHR\tAUX\tBatt\tAirSpd\tSetSpd\t\n";
    addRows(d, 498, 635); addRows(d, 67, 725); addRows(d, 22, 135); addRows(d, 24, 115); addRows(d, 497, 497);
    d += "DATA END\n1,HS,OK\r";
    XloResult r = parseXlo(d.data(), d.size());
    TEST_ASSERT_EQUAL_UINT32(0, r.badLines);
    TEST_ASSERT_TRUE(r.wrapJoined);
    TEST_ASSERT_EQUAL_UINT32(4, r.runs.size());
    TEST_ASSERT_EQUAL_INT32(497, r.runs[0].rows.front().time);
    TEST_ASSERT_EQUAL_INT32(635, r.runs[0].rows.back().time);
    TEST_ASSERT_EQUAL_INT32(24, r.runs.back().rows.front().time);    // newest run = C
    TEST_ASSERT_EQUAL_INT32(115, r.runs.back().rows.back().time);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parses_all_rows);
    RUN_TEST(test_splits_runs_and_rejoins_wrap);
    RUN_TEST(test_last_flight_summary_matches_workbook);
    RUN_TEST(test_aux_and_batt_are_separate_fields);
    RUN_TEST(test_wrap_row_later_than_newest_run);
    return UNITY_END();
}
