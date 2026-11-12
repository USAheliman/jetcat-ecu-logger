#pragma once
// Flight storage on LittleFS. Each flight = three files in /flights:
//   <key>.json  summary + every HS reply as received
//   <key>.csv   most recent run from the log, oldest row first
//   <key>.raw   the complete XLO dump exactly as received
// key = "<ECU serial>-<runs OK>", e.g. "3934-0555". A key already present = duplicate.
#include <Arduino.h>
#include <vector>

namespace store {
bool begin();
bool exists(const String& key);
bool write(const String& path, const uint8_t* data, size_t len);
inline bool write(const String& path, const String& text) { return write(path, (const uint8_t*)text.c_str(), text.length()); }
std::vector<String> listKeys();
bool printFile(const String& path, Print& out);
bool removeFlight(const String& key);
void removeAll();
size_t usedBytes();
size_t totalBytes();

// Whole file as text ("" if missing). For files up to ~200 KB.
String readText(const String& path);
// Key looks like "<digits>-<digits>" (guards file paths built from API input).
bool validKey(const String& key);

// Heli names: ECU serial -> name, kept in /config/helis.txt as "serial=name" lines.
String helisJson();                                  // {"3934":"BO 105", ...}
bool setHeliName(const String& serial, const String& name);   // empty name = remove
}  // namespace store
