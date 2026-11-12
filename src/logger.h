#pragma once
// Shared state between the download state machine (main.cpp) and the API (api.cpp).
#include <Arduino.h>
#include <time.h>

extern String   g_lastResult;          // last download result, for status
extern volatile bool g_downloading;    // true while talking to the ECU
extern volatile bool g_downloadRequested;
extern size_t   g_dumpBytes;           // bytes received so far in the current XLO dump
extern const char* FW;

const char* stateName();
bool clockValid();
void setClock(time_t epoch, const char* source);   // system clock + RTC if fitted
bool rtcPresent();
