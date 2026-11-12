// Host test: pio test -e native
// Parses the real dump captured from the BO 105 ECU (serial 3934) on 1 Nov 2026
// and checks the values against the Excel workbook made from the same dump.
#include <unity.h>
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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parses_all_rows);
    RUN_TEST(test_splits_runs_and_rejoins_wrap);
    RUN_TEST(test_last_flight_summary_matches_workbook);
    RUN_TEST(test_aux_and_batt_are_separate_fields);
    return UNITY_END();
}
