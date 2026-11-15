#include "store.h"
#include <LittleFS.h>

namespace store {

static const char* DIR = "/flights";

bool begin() {
    if (!LittleFS.begin(true)) return false;   // formats on first use
    if (!LittleFS.exists(DIR)) LittleFS.mkdir(DIR);
    return true;
}

bool exists(const String& key) {
    return LittleFS.exists(String(DIR) + "/" + key + ".json");
}

bool write(const String& path, const uint8_t* data, size_t len) {
    String tmp = path + ".tmp";
    File f = LittleFS.open(tmp, "w");
    if (!f) return false;
    size_t n = f.write(data, len);
    f.close();
    if (n != len) { LittleFS.remove(tmp); return false; }
    LittleFS.remove(path);
    return LittleFS.rename(tmp, path);   // a flight is never left half-written
}

std::vector<String> listKeys() {
    std::vector<String> keys;
    File d = LittleFS.open(DIR);
    if (!d) return keys;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        String n = f.name();
        int slash = n.lastIndexOf('/');
        if (slash >= 0) n = n.substring(slash + 1);
        if (n.endsWith(".json")) keys.push_back(n.substring(0, n.length() - 5));
    }
    return keys;
}

bool printFile(const String& path, Print& out) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    uint8_t buf[512];
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        out.write(buf, n);
    }
    f.close();
    return true;
}

bool removeFlight(const String& key) {
    bool any = false;
    for (const char* ext : {".json", ".csv", ".raw"}) {
        String p = String(DIR) + "/" + key + ext;
        if (LittleFS.exists(p)) { LittleFS.remove(p); any = true; }
    }
    return any;
}

void removeAll() {
    for (const String& k : listKeys()) removeFlight(k);
}

size_t usedBytes() { return LittleFS.usedBytes(); }
size_t totalBytes() { return LittleFS.totalBytes(); }

String readText(const String& path) {
    File f = LittleFS.open(path, "r");
    if (!f) return String();
    String s;
    s.reserve(f.size() + 1);
    uint8_t buf[512];
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        s.concat((const char*)buf, n);
    }
    f.close();
    return s;
}

bool validKey(const String& key) {
    int dash = key.indexOf('-');
    if (dash <= 0 || dash == (int)key.length() - 1 || key.length() > 24) return false;
    for (int i = 0; i < (int)key.length(); ++i)
        if (i != dash && !isDigit(key[i])) return false;
    return true;
}

static const char* HELIS = "/config/helis.txt";

static std::vector<std::pair<String, String>> loadHelis() {
    std::vector<std::pair<String, String>> v;
    String t = readText(HELIS);
    int start = 0;
    while (start < (int)t.length()) {
        int nl = t.indexOf('\n', start);
        String line = nl < 0 ? t.substring(start) : t.substring(start, nl);
        int eq = line.indexOf('=');
        if (eq > 0) v.push_back({line.substring(0, eq), line.substring(eq + 1)});
        if (nl < 0) break;
        start = nl + 1;
    }
    return v;
}

static String jsonEsc(const String& s) {
    String o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((uint8_t)c >= 0x20) o += c;
    }
    return o;
}

String helisJson() {
    String j = "{";
    bool first = true;
    for (auto& p : loadHelis()) {
        if (!first) j += ",";
        j += "\"" + jsonEsc(p.first) + "\":\"" + jsonEsc(p.second) + "\"";
        first = false;
    }
    return j + "}";
}

bool setHeliName(const String& serial, const String& name) {
    for (char c : serial) if (!isDigit(c)) return false;
    if (!serial.length() || name.length() > 40) return false;
    String clean;
    for (char c : name) if (c != '\n' && c != '\r' && c != '=') clean += c;
    auto v = loadHelis();
    bool found = false;
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->first == serial) {
            if (clean.length()) it->second = clean; else v.erase(it);
            found = true;
            break;
        }
    }
    if (!found && clean.length()) v.push_back({serial, clean});
    String out;
    for (auto& p : v) out += p.first + "=" + p.second + "\n";
    if (!LittleFS.exists("/config")) LittleFS.mkdir("/config");
    return write(HELIS, out);
}

// ---------------------------------------------------------------- service details + photos
static const char* PHOTOS = "/photos";

bool validSerial(const String& serial) {
    if (!serial.length() || serial.length() > 10) return false;
    for (char c : serial) if (!isDigit(c)) return false;
    return true;
}

static String infoPath(const String& serial) { return "/config/heli_" + serial + ".txt"; }
String photoPath(const String& serial) { return String(PHOTOS) + "/" + serial + ".b64"; }

static String infoValue(const String& text, const char* key) {
    String k = String(key) + "=";
    int start = 0;
    while (start < (int)text.length()) {
        int nl = text.indexOf('\n', start);
        String line = nl < 0 ? text.substring(start) : text.substring(start, nl);
        if (line.startsWith(k)) return line.substring(k.length());
        if (nl < 0) break;
        start = nl + 1;
    }
    return String();
}

static bool cleanNumber(const String& in, String& out) {   // "" or a plain non-negative number
    out = "";
    for (char c : in) {
        if (isDigit(c) || c == '.') out += c;
        else if (c != ' ') return false;
    }
    return out.length() < 12;
}

String heliInfoJson() {
    std::vector<String> serials;
    auto addSerial = [&](const String& s) {
        if (!validSerial(s)) return;
        for (auto& x : serials) if (x == s) return;
        serials.push_back(s);
    };
    for (const char* dirName : {"/config", PHOTOS}) {
        File d = LittleFS.open(dirName);
        if (!d) continue;
        for (File f = d.openNextFile(); f; f = d.openNextFile()) {
            String n = f.name();
            int slash = n.lastIndexOf('/');
            if (slash >= 0) n = n.substring(slash + 1);
            if (n.startsWith("heli_") && n.endsWith(".txt")) addSerial(n.substring(5, n.length() - 4));
            else if (n.endsWith(".b64")) addSerial(n.substring(0, n.length() - 4));
        }
    }
    String j = "{";
    bool first = true;
    for (auto& s : serials) {
        String t = readText(infoPath(s));
        String sh = infoValue(t, "service_h"), sd = infoValue(t, "service_date"), iv = infoValue(t, "interval_h");
        size_t plen = 0;
        File pf = LittleFS.open(photoPath(s), "r");
        if (pf) { plen = pf.size(); pf.close(); }
        if (!first) j += ",";
        first = false;
        j += "\"" + s + "\":{";
        j += "\"service_h\":" + (sh.length() ? sh : String("null"));
        j += ",\"service_date\":" + (sd.length() ? "\"" + jsonEsc(sd) + "\"" : String("null"));
        j += ",\"interval_h\":" + (iv.length() ? iv : String("null"));
        j += ",\"photo_len\":" + String((unsigned)plen) + "}";
    }
    return j + "}";
}

bool setHeliInfo(const String& serial, const String& serviceH, const String& serviceDate, const String& intervalH) {
    if (!validSerial(serial)) return false;
    String sh, iv;
    if (!cleanNumber(serviceH, sh) || !cleanNumber(intervalH, iv)) return false;
    String sd;
    for (char c : serviceDate) if (isDigit(c) || c == '-') sd += c;
    if (sd.length() > 10) return false;
    if (!LittleFS.exists("/config")) LittleFS.mkdir("/config");
    if (!sh.length() && !sd.length() && !iv.length()) { LittleFS.remove(infoPath(serial)); return true; }
    String t = "service_h=" + sh + "\nservice_date=" + sd + "\ninterval_h=" + iv + "\n";
    return write(infoPath(serial), t);
}

bool photoChunk(const String& serial, int idx, int total, const String& b64) {
    if (!validSerial(serial) || idx < 0 || total < 1 || idx >= total || total > 64) return false;
    for (char c : b64) if (!(isAlphaNumeric(c) || c == '+' || c == '/' || c == '=')) return false;
    if (!LittleFS.exists(PHOTOS)) LittleFS.mkdir(PHOTOS);
    String tmp = photoPath(serial) + ".part";
    File f = LittleFS.open(tmp, idx == 0 ? "w" : "a");
    if (!f) return false;
    size_t n = f.print(b64);
    f.close();
    if (n != b64.length()) { LittleFS.remove(tmp); return false; }
    if (idx == total - 1) {
        LittleFS.remove(photoPath(serial));
        return LittleFS.rename(tmp, photoPath(serial));
    }
    return true;
}

bool removePhoto(const String& serial) {
    if (!validSerial(serial)) return false;
    LittleFS.remove(photoPath(serial) + ".part");
    return LittleFS.remove(photoPath(serial)) || true;
}

}  // namespace store
