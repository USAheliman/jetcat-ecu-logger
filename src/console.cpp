#include "console.h"
#include <time.h>

ConsoleOut Con;

void ConsoleOut::begin(size_t bytes) {
    ring = (char*)ps_malloc(bytes);
    if (!ring) { bytes = 16 * 1024; ring = (char*)malloc(bytes); }   // no PSRAM: smaller buffer
    cap = ring ? bytes : 0;
}

// Each line in the ring starts with the time: UTC clock time once the clock is set,
// otherwise seconds since boot (e.g. "+12.3s"). USB serial output is not stamped, because
// the PlatformIO monitor adds its own time stamps.
static size_t makeStamp(char* out, size_t n) {
    time_t t = time(nullptr);
    if (t > 1700000000) {
        struct tm tmv;
        gmtime_r(&t, &tmv);
        return strftime(out, n, "%H:%M:%S ", &tmv);
    }
    uint32_t ms = millis();
    return snprintf(out, n, "+%lu.%lus ", (unsigned long)(ms / 1000), (unsigned long)((ms / 100) % 10));
}

void ConsoleOut::ringPut(const uint8_t* buf, size_t len) {
    if (!cap || !len) return;
    char stamp[24];
    size_t stampLen = makeStamp(stamp, sizeof(stamp));   // outside the lock: may take a little time
    portENTER_CRITICAL(&mux);
    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if (c == '\r') continue;   // lines end with \n only
        if (atLineStart) {
            for (size_t k = 0; k < stampLen; k++) { ring[written % cap] = stamp[k]; written++; }
            atLineStart = false;
        }
        ring[written % cap] = (char)c;
        written++;
        if (c == '\n') atLineStart = true;
    }
    portEXIT_CRITICAL(&mux);
}

size_t ConsoleOut::write(uint8_t c) { return write(&c, 1); }

size_t ConsoleOut::write(const uint8_t* buf, size_t len) {
    Serial.write(buf, len);
    ringPut(buf, len);
    return len;
}

void ConsoleOut::note(const String& s) { ringPut((const uint8_t*)s.c_str(), s.length()); }

uint32_t ConsoleOut::total() {
    portENTER_CRITICAL(&mux);
    uint32_t w = written;
    portEXIT_CRITICAL(&mux);
    return w;
}

String ConsoleOut::read(uint32_t since, size_t maxLen, uint32_t* next, uint32_t* first) {
    String out;
    if (!cap) { *next = 0; *first = 0; return out; }
    // Copy under the lock into a plain buffer first; String allocation stays outside it.
    char* tmp = (char*)malloc(maxLen + 1);
    if (!tmp) { *next = since; *first = since; return out; }
    size_t n = 0;
    portENTER_CRITICAL(&mux);
    uint32_t oldest = written > cap ? written - cap : 0;
    if (since < oldest || since > written) since = oldest;
    n = written - since;
    if (n > maxLen) n = maxLen;
    for (size_t i = 0; i < n; i++) tmp[i] = ring[(since + i) % cap];
    *first = oldest;
    portEXIT_CRITICAL(&mux);
    tmp[n] = 0;
    *next = since + n;
    out = tmp;
    free(tmp);
    return out;
}

// ---------------------------------------------------------------- command queue
// Requests from WiFi and Bluetooth are both handled inside net::poll() on the loop task,
// and commands are taken in loop(), so the queue is only ever touched from one task.
namespace console {
static String queue[4];
static uint8_t qHead = 0, qCount = 0;

bool queueCommand(const String& cmd) {
    String c = cmd.substring(0, MAX_CMD_LEN);
    c.trim();
    if (!c.length() || qCount >= 4) return false;
    queue[(qHead + qCount) % 4] = c;
    qCount++;
    return true;
}

bool takeCommand(String& cmd) {
    if (!qCount) return false;
    cmd = queue[qHead];
    queue[qHead] = String();
    qHead = (qHead + 1) % 4;
    qCount--;
    return true;
}
}  // namespace console
