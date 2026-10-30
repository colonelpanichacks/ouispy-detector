#include "wardrive.h"
#include "devinfo.h"
#include <Preferences.h>
#include "gpsnode.h"

// Sightings travel from the radio callbacks to loop() through a small ring (two
// producers, so a spinlock), are de-duplicated against a table of every device seen,
// and printed. The callbacks also drop a device heard again within WD_HOLD_MS before
// any parsing: an access point beacons ~10 times a second.

#define WD_RING        96
#define WD_RAW_RING    24
#define WD_TABLE       2048       // devices remembered (oldest replaced when full)
#define WD_HOLD_MS     2000       // callback-side: ignore the same device this soon
#define WD_REPEAT_MS   10000      // re-print a device still around this often
#define WD_STRONGER_DB 8          // ...or right away when this much stronger

enum : uint8_t { WD_WIFI = 1, WD_BLE = 2 };

struct WdObs {
    uint8_t kind;
    uint8_t mac[6];
    int8_t rssi;
    uint8_t ch;        // WiFi channel
    uint8_t sec;       // WiFi: DI_SEC value, 255 unknown
    uint8_t wcap;      // WiFi: DI_WCAP bits
    uint8_t rnd;       // BLE: random address
    bool hasMfr;
    uint16_t mfr;      // BLE company id
    uint16_t appear;   // BLE GAP appearance
    char txt[33];      // SSID or BLE name
    // Intel: BLE service / service-data UUIDs, TX power, flags; WiFi vendor IE OUIs,
    // country, top legacy rate.
    uint16_t uuid[6];
    uint8_t nuuid;
    int8_t txp;        // BLE advertised TX power, 127 none
    uint8_t adFlags;   // BLE AD flags (0x01), 0 none
    uint8_t vend[9];   // up to 3 vendor IE OUIs
    uint8_t nvend;
    char cc[3];
    uint8_t rate;      // units of 500 kb/s
};

struct WdRaw {
    uint8_t kind;
    uint8_t sub;       // WiFi mgmt subtype
    uint8_t ch;
    int8_t rssi;
    uint8_t rnd;
    uint8_t mac[6];    // BLE address
    uint16_t len;      // full length heard
    uint8_t n;         // bytes kept
    uint8_t data[112];
};

static WdObs ring[WD_RING];
static volatile uint16_t rHead = 0, rTail = 0;
static WdRaw rawRing[WD_RAW_RING];
static volatile uint16_t qHead = 0, qTail = 0;
static portMUX_TYPE wdMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t dropped = 0, rawDropped = 0;
static volatile bool rawOn = false;
// Always on: a wardriver is always wardriving (no switch).
static constexpr int8_t active = 1;

bool wdActive() { return wdOn(); }

// Callback-side hold: hash of (kind, mac) -> last time queued. Races are harmless
// (at worst a duplicate gets through).
static uint32_t holdKey[512], holdMs[512];

static uint32_t macKey(uint8_t kind, const uint8_t* m) {
    uint32_t h = 2166136261u ^ kind;
    for (int i = 0; i < 6; i++) h = (h ^ m[i]) * 16777619u;
    return h ? h : 1;
}

static bool held(uint8_t kind, const uint8_t* mac) {
    uint32_t k = macKey(kind, mac), now = millis();
    uint16_t s = k & 511;
    if (holdKey[s] == k && now - holdMs[s] < WD_HOLD_MS) return true;
    holdKey[s] = k;
    holdMs[s] = now;
    return false;
}

static void push(const WdObs& o) {
    portENTER_CRITICAL_SAFE(&wdMux);
    uint16_t next = (rHead + 1) % WD_RING;
    if (next == rTail) dropped++;
    else ring[rHead] = o, rHead = next;
    portEXIT_CRITICAL_SAFE(&wdMux);
}

static void pushRaw(const WdRaw& r) {
    portENTER_CRITICAL_SAFE(&wdMux);
    uint16_t next = (qHead + 1) % WD_RAW_RING;
    if (next == qTail) rawDropped++;
    else rawRing[qHead] = r, qHead = next;
    portEXIT_CRITICAL_SAFE(&wdMux);
}

void wdWifiFrame(const uint8_t* f, int len, uint8_t subtype, int8_t rssi, uint8_t rxChan) {
    if (len < 24 || active != 1) return;
    if (rawOn) {
        WdRaw r = {};
        r.kind = WD_WIFI, r.sub = subtype, r.ch = rxChan, r.rssi = rssi, r.len = len;
        r.n = len > (int)sizeof r.data ? sizeof r.data : len;
        memcpy(r.data, f, r.n);
        pushRaw(r);
    }
    // Access points: beacons and probe responses (addr2 = the AP's BSSID).
    if (subtype != 0x08 && subtype != 0x05) return;
    if (held(WD_WIFI, f + 10)) return;
    DevInfo di;
    devinfoFromWifi(di, f, len, subtype, rxChan);
    WdObs o = {};
    o.kind = WD_WIFI;
    memcpy(o.mac, f + 10, 6);
    o.rssi = rssi;
    o.ch = rxChan;
    o.sec = 255;
    for (uint8_t i = 0; i + 2 <= di.n; i += 2 + di.b[i + 1]) {
        uint8_t t = di.b[i], l = di.b[i + 1];
        const uint8_t* v = di.b + i + 2;
        if (t == DI_SSID) { uint8_t k = l > 32 ? 32 : l; memcpy(o.txt, v, k); o.txt[k] = 0; }
        else if (t == DI_CHAN && l) o.ch = v[0];
        else if (t == DI_SEC && l) o.sec = v[0];
        else if (t == DI_WCAP && l) o.wcap = v[0];
        else if (t == DI_RATE && l) o.rate = v[0];
        else if (t == DI_COUNTRY && l >= 2) o.cc[0] = v[0], o.cc[1] = v[1], o.cc[2] = 0;
        else if (t == DI_VENDOR) { o.nvend = l / 3 > 3 ? 3 : l / 3; memcpy(o.vend, v, o.nvend * 3); }
    }
    push(o);
}

void wdBleAdvert(const uint8_t mac[6], bool randomAddr, int8_t rssi, const char* name,
                 bool hasMfr, uint16_t mfr, uint16_t appearance, const uint8_t* adv, int advLen) {
    if (active != 1) return;
    if (rawOn) {
        WdRaw r = {};
        r.kind = WD_BLE, r.rssi = rssi, r.rnd = randomAddr, r.len = advLen;
        memcpy(r.mac, mac, 6);
        r.n = advLen > (int)sizeof r.data ? sizeof r.data : (advLen < 0 ? 0 : advLen);
        if (adv && r.n) memcpy(r.data, adv, r.n);
        pushRaw(r);
    }
    if (held(WD_BLE, mac)) return;
    WdObs o = {};
    o.kind = WD_BLE;
    memcpy(o.mac, mac, 6);
    o.rssi = rssi;
    o.rnd = randomAddr;
    o.hasMfr = hasMfr;
    o.mfr = mfr;
    o.appear = appearance;
    o.txp = 127;
    if (name) strlcpy(o.txt, name, sizeof o.txt);
    // AD structures: flags, 16-bit service UUIDs (complete / incomplete), service data
    // UUIDs, TX power.
    for (int i = 0; adv && i + 1 < advLen;) {
        uint8_t l = adv[i];
        if (!l || i + 1 + l > advLen) break;
        uint8_t t = adv[i + 1];
        const uint8_t* v = adv + i + 2;
        int vl = l - 1;
        if (t == 0x01 && vl >= 1) o.adFlags = v[0];
        else if (t == 0x0A && vl >= 1) o.txp = (int8_t)v[0];
        else if ((t == 0x02 || t == 0x03) && vl >= 2)
            for (int k = 0; k + 1 < vl && o.nuuid < 6; k += 2) o.uuid[o.nuuid++] = v[k] | (v[k + 1] << 8);
        else if (t == 0x16 && vl >= 2 && o.nuuid < 6) o.uuid[o.nuuid++] = v[0] | (v[1] << 8);
        i += 1 + l;
    }
    push(o);
}

// ---------------------------------------------------------------- loop side

struct WdSeen { uint32_t key; uint32_t lastMs; int8_t lastRssi; };
static WdSeen* seen = nullptr;   // WD_TABLE entries, allocated on first use
static uint32_t nWifi = 0, nBle = 0;

// Find or claim the device's slot; isNew when it was not there.
static WdSeen* lookup(uint32_t key, bool& isNew) {
    if (!seen) {
        seen = (WdSeen*)calloc(WD_TABLE, sizeof(WdSeen));
        if (!seen) return nullptr;
    }
    uint32_t s = key % WD_TABLE, oldest = s;
    for (int i = 0; i < 24; i++, s = (s + 1) % WD_TABLE) {
        if (seen[s].key == key) { isNew = false; return &seen[s]; }
        if (!seen[s].key) { isNew = true; seen[s].key = key; return &seen[s]; }
        if ((int32_t)(seen[oldest].lastMs - seen[s].lastMs) > 0) oldest = s;
    }
    isNew = true;   // neighbourhood full: replace the one heard longest ago
    seen[oldest] = {key, 0, 0};
    return &seen[oldest];
}

static void jsonStr(char* out, size_t cap, const char* s) {
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        uint8_t c = (uint8_t)*s;
        if (c == '"' || c == '\\') out[n++] = '\\', out[n++] = c;
        else if (c < 0x20) n += snprintf(out + n, cap - n, "\\u%04x", c);
        else out[n++] = c;   // UTF-8 passes through
    }
    out[n] = 0;
}

static const char* secName(uint8_t s) {
    switch (s) {
        case 0: return "open";
        case 1: return "WEP";
        case 2: return "WPA";
        case 3: return "WPA2";
        case 4: return "WPA3";
        case 5: return "WPA2/WPA3";
        default: return "";
    }
}


static void printObs(const WdObs& o, bool isNew) {
    char mac[18], txt[200];
    // A GPS node knows where it is: the sighting carries its own fix.
    char loc[64] = "";
    double la, lo;
    float acc;
    if (gpsNodeFix(la, lo, acc)) snprintf(loc, sizeof loc, ",\"lat\":%.7f,\"lon\":%.7f,\"acc\":%.1f", la, lo, acc);
    snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", o.mac[0], o.mac[1], o.mac[2], o.mac[3], o.mac[4], o.mac[5]);
    jsonStr(txt, sizeof txt, o.txt);
    if (o.kind == WD_WIFI) {
        char extra[96] = "";
        int n = 0;
        const char* std = (o.wcap & 4) ? "ax" : (o.wcap & 2) ? "ac" : (o.wcap & 1) ? "n" : "";
        if (*std) n += snprintf(extra + n, sizeof extra - n, ",\"std\":\"%s\"", std);
        if (o.rate) n += snprintf(extra + n, sizeof extra - n, ",\"rate\":%u", o.rate / 2);
        if (o.cc[0] >= 'A' && o.cc[0] <= 'Z') n += snprintf(extra + n, sizeof extra - n, ",\"cc\":\"%c%c\"", o.cc[0], o.cc[1]);
        if (o.nvend) {
            n += snprintf(extra + n, sizeof extra - n, ",\"vend\":[");
            for (int k = 0; k < o.nvend; k++)
                n += snprintf(extra + n, sizeof extra - n, "%s\"%02X%02X%02X\"", k ? "," : "", o.vend[3 * k], o.vend[3 * k + 1], o.vend[3 * k + 2]);
            n += snprintf(extra + n, sizeof extra - n, "]");
        }
        Serial.printf("WD {\"t\":\"wifi\",\"mac\":\"%s\",\"ssid\":\"%s\",\"ch\":%u,\"rssi\":%d,\"sec\":\"%s\","
                      "\"hidden\":%d,\"wps\":%d%s%s,\"new\":%d}\n",
                      mac, txt, o.ch, o.rssi, secName(o.sec), (o.wcap & 16) ? 1 : 0, (o.wcap & 8) ? 1 : 0, extra, loc, isNew ? 1 : 0);
    } else {
        char extra[140] = "";
        int n = 0;
        if (o.hasMfr) n += snprintf(extra + n, sizeof extra - n, ",\"mfr\":%u", o.mfr);
        if (o.appear) n += snprintf(extra + n, sizeof extra - n, ",\"app\":%u", o.appear);
        if (o.txp != 127) n += snprintf(extra + n, sizeof extra - n, ",\"tx\":%d", o.txp);
        if (o.adFlags) n += snprintf(extra + n, sizeof extra - n, ",\"fl\":%u", o.adFlags);
        if (o.nuuid) {
            n += snprintf(extra + n, sizeof extra - n, ",\"svc\":[");
            for (int k = 0; k < o.nuuid; k++) n += snprintf(extra + n, sizeof extra - n, "%s\"%04X\"", k ? "," : "", o.uuid[k]);
            n += snprintf(extra + n, sizeof extra - n, "]");
        }
        Serial.printf("WD {\"t\":\"ble\",\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"rnd\":%d%s%s,\"new\":%d}\n",
                      mac, txt, o.rssi, o.rnd, extra, loc, isNew ? 1 : 0);
    }
}

static void printRaw(const WdRaw& r) {
    static const char* hx = "0123456789abcdef";
    char hex[sizeof r.data * 2 + 1];
    for (int i = 0; i < r.n; i++) hex[2 * i] = hx[r.data[i] >> 4], hex[2 * i + 1] = hx[r.data[i] & 15];
    hex[2 * r.n] = 0;
    auto m = [](char* o, const uint8_t* a) {
        snprintf(o, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
    };
    if (r.kind == WD_WIFI) {
        char a1[18], a2[18], a3[18];
        m(a1, r.data + 4), m(a2, r.data + 10), m(a3, r.data + 16);
        Serial.printf("RAW {\"t\":\"wifi\",\"st\":%u,\"ch\":%u,\"rssi\":%d,\"a1\":\"%s\",\"a2\":\"%s\",\"a3\":\"%s\","
                      "\"len\":%u,\"hex\":\"%s\"}\n", r.sub, r.ch, r.rssi, a1, a2, a3, r.len, hex);
    } else {
        char a[18];
        m(a, r.mac);
        Serial.printf("RAW {\"t\":\"ble\",\"mac\":\"%s\",\"rssi\":%d,\"rnd\":%u,\"len\":%u,\"hex\":\"%s\"}\n",
                      a, r.rssi, r.rnd, r.len, hex);
    }
}

void wdLoop() {
    // A bounded batch per pass, and only while USB has room: a busy or absent host
    // must never stall the scan loop (sightings just wait in the ring, or drop).
    for (int i = 0; i < 12; i++) {
        WdObs o;
        portENTER_CRITICAL(&wdMux);
        bool any = rTail != rHead;
        if (any) o = ring[rTail], rTail = (rTail + 1) % WD_RING;
        portEXIT_CRITICAL(&wdMux);
        if (!any) break;
        bool isNew;
        WdSeen* s = lookup(macKey(o.kind, o.mac), isNew);
        if (!s) break;
        uint32_t now = millis();
        if (isNew) (o.kind == WD_WIFI ? nWifi : nBle)++;
        if (!isNew && now - s->lastMs < WD_REPEAT_MS && o.rssi < s->lastRssi + WD_STRONGER_DB) continue;
        s->lastMs = now;
        s->lastRssi = o.rssi;
        if (Serial && Serial.availableForWrite() > 200) printObs(o, isNew);
    }
    for (int i = 0; i < 6; i++) {
        WdRaw r;
        portENTER_CRITICAL(&wdMux);
        bool any = qTail != qHead;
        if (any) r = rawRing[qTail], qTail = (qTail + 1) % WD_RAW_RING;
        portEXIT_CRITICAL(&wdMux);
        if (!any) break;
        if (Serial && Serial.availableForWrite() > 300) printRaw(r);
    }
}

// ---------------------------------------------------------------- channel plan

static uint16_t chMask = 0;   // bit n = channel n (1..13); 0 = not loaded yet
static const uint16_t CH_ALL = 0x3FFE, CH_DEFAULT = 0x0FFE;   // 1-13, 1-11

static uint16_t mask() {
    if (!chMask) {
        Preferences p;
        p.begin("wd", true);
        chMask = p.getUShort("ch", CH_DEFAULT) & CH_ALL;
        p.end();
        if (!chMask) chMask = CH_DEFAULT;
    }
    return chMask;
}

uint8_t wdFirstChannel() {
    for (int c = 13; c >= 1; c--) if (mask() & (1 << c)) return c;
    return 11;
}

uint8_t wdNextChannel(uint8_t cur) {   // the next lower channel in the plan, wrapping
    for (int k = 1; k <= 13; k++) {
        int c = cur - k;
        if (c < 1) c += 13;
        if (mask() & (1 << c)) return c;
    }
    return cur;
}

void wdChannelsText(char* out, size_t cap) {
    size_t n = 0;
    out[0] = 0;
    uint16_t m = mask();
    for (int c = 1; c <= 13; c++) {
        if (!(m & (1 << c))) continue;
        int e = c;
        while (e < 13 && (m & (1 << (e + 1)))) e++;
        n += snprintf(out + n, cap > n ? cap - n : 0, "%s%d", n ? "," : "", c);
        if (e > c) n += snprintf(out + n, cap > n ? cap - n : 0, "%s%d", e > c + 1 ? "-" : ",", e);
        c = e;
    }
}

// "all", "6", "1-6", "1,6,11", "1-3,11": the channels as a bit mask, 0 when invalid.
static uint16_t parseChannels(String s) {
    s.trim();
    if (s == "all") return CH_ALL;
    uint16_t m = 0;
    while (s.length()) {
        int comma = s.indexOf(',');
        String part = comma < 0 ? s : s.substring(0, comma);
        s = comma < 0 ? String("") : s.substring(comma + 1);
        part.trim();
        int dash = part.indexOf('-');
        int a = (dash < 0 ? part : part.substring(0, dash)).toInt();
        int b = dash < 0 ? a : part.substring(dash + 1).toInt();
        if (a < 1 || b > 13 || a > b) return 0;
        for (int c = a; c <= b; c++) m |= 1 << c;
    }
    return m;
}

static void saveMask(uint16_t m) {
    chMask = m;
    Preferences p;
    p.begin("wd", false);
    p.putUShort("ch", m);
    p.end();
}

bool wdMeshPayload(const String& words, uint8_t out[4]) {
    out[0] = out[1] = 255, out[2] = out[3] = 0;
    String w = words;
    w.trim();
    if (w == "raw on" || w == "raw off") out[1] = w == "raw on";
    else if (w.startsWith("ch ")) {
        uint16_t m = parseChannels(w.substring(3));
        if (!m) return false;
        out[2] = m & 0xFF, out[3] = m >> 8;
    } else return false;
    return true;
}

bool wdApplyMesh(const uint8_t* p, uint16_t len) {
    if (!wdOn() || len < 4) return false;
    uint16_t m = p[2] | (p[3] << 8);
    if (m & ~CH_ALL) return false;
    if (m) saveMask(m);
    if (p[1] != 255) rawOn = p[1] == 1;
    return true;
}

bool wdConsole(const String& verb, const String& arg) {
    if (verb != "wd") return false;
    if (!wdOn()) {
        Serial.println("ERR wd: not a wardriver (flash the wardrive build)");
        return true;
    }
    if (arg.startsWith("ch")) {
        String spec = arg.substring(2);
        spec.trim();
        char t[40];
        if (spec.length()) {
            uint16_t m = parseChannels(spec);
            if (!m) {
                Serial.println("ERR usage: wd ch all | 6 | 1-6 | 1,6,11");
                return true;
            }
            saveMask(m);
        }
        wdChannelsText(t, sizeof t);
        Serial.printf("OK wd ch %s\n", t);
        return true;
    }
    if (arg == "raw on" || arg == "raw off") {
        rawOn = arg == "raw on";
        Serial.printf("OK wd raw %s\n", rawOn ? "on" : "off");
    } else {
        char t[40];
        wdChannelsText(t, sizeof t);
        Serial.printf("OK wd %s wifi=%lu ble=%lu ch=%s dropped=%lu raw=%s raw_dropped=%lu\n", wdActive() ? "on" : "off",
                      (unsigned long)nWifi, (unsigned long)nBle, t, (unsigned long)dropped, rawOn ? "on" : "off",
                      (unsigned long)rawDropped);
    }
    return true;
}

uint32_t wdWifiCount() { return nWifi; }
uint32_t wdBleCount() { return nBle; }
