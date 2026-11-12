#pragma once
// Remote console. Everything the firmware prints goes through `Con`: it is written to the
// USB serial port exactly as before, and also kept in a ring buffer (64 KB in PSRAM) so the
// recent output can be read over WiFi or Bluetooth (GET /api/console). Commands can be
// sent the same way (POST /api/console) and run through the same handler as the serial
// console, so a remote command can do nothing a typed one can't.
#include <Arduino.h>

class ConsoleOut : public Print {
public:
    void begin(size_t bytes);
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t len) override;

    // Adds text to the ring buffer only (not to USB serial). Used to record commands typed
    // on the serial monitor, which already echoes them locally.
    void note(const String& s);

    // Positions count every byte ever written, so a reader asks for "everything after N".
    // Returns up to maxLen bytes from `since` (or from the oldest byte still held, if
    // `since` has already been overwritten). *next = position after the returned text,
    // *first = oldest position still held.
    String read(uint32_t since, size_t maxLen, uint32_t* next, uint32_t* first);
    uint32_t total();
    size_t capacity() const { return cap; }

private:
    void ringPut(const uint8_t* buf, size_t len);
    char* ring = nullptr;
    size_t cap = 0;
    uint32_t written = 0;     // total bytes ever put in the ring
    bool atLineStart = true;
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};

extern ConsoleOut Con;

namespace console {
constexpr size_t MAX_CMD_LEN = 120;
// Loop task only (API requests are handled in net::poll()).
bool queueCommand(const String& cmd);   // false if empty or the queue (4) is full
bool takeCommand(String& cmd);          // one queued command at a time
}
