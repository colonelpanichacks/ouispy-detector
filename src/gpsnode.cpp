#include "gpsnode.h"
#ifdef GPS_NODE
#include <M5GFX.h>
#include <Preferences.h>
#include "mesh.h"
#include "wardrive.h"

// Atomic GPS Base on an AtomS3: GPS NMEA at 9600 on G5. Its microSD slot is left unused.
#define GPS_RX 5
#define BTN 41

namespace {

M5GFX lcd;
M5Canvas fb(&lcd);

struct Fix {
    bool valid;
    double lat, lon;
    float alt, acc, hdop, spd, crs;
    uint8_t sats;
    uint32_t ms;
} fix = {}, pend = {};
uint8_t satsSeen = 0;
// Satellites in view (GSV): their signal (dB-Hz, 0 = not tracked), for the bars.
struct Sat { uint8_t prn, snr; char sys; };
Sat sats[24];
uint8_t nsats = 0, gsvBuild = 0;
Sat gsvTmp[24];
uint8_t page = 0;   // 0 satellites, 1 node (tap the screen)
bool dark = false;  // incognito: screen off (double-tap; kept across reboots)

void setDark(bool on) {
    dark = on;
    if (on) { lcd.setBrightness(0); lcd.sleep(); }
    else { lcd.wakeup(); lcd.setBrightness(110); }
    Preferences p;
    p.begin("gpsnode", false);
    p.putBool("dark", on);
    p.end();
}
char nmea[100];
int nlen = 0;
uint32_t nmeaGood = 0;   // sentences with a good checksum (the module is talking)

bool nmea_ok(const char* s) {
    if (s[0] != '$') return false;
    const char* star = strchr(s, '*');
    if (!star || !isxdigit((unsigned char)star[1]) || !isxdigit((unsigned char)star[2])) return false;
    uint8_t x = 0;
    for (const char* p = s + 1; p < star; p++) x ^= (uint8_t)*p;
    return x == (uint8_t)strtol(star + 1, nullptr, 16);
}

double coord(const char* v, const char* h) {
    if (!*v) return NAN;
    double x = atof(v);
    int d = (int)(x / 100);
    double r = d + (x - d * 100) / 60.0;
    return (*h == 'S' || *h == 'W') ? -r : r;
}

void sentence(char* s) {
    char* f[24];
    int n = 0;
    char* star = strchr(s, '*');
    if (star) *star = 0;
    f[n++] = s;
    for (char* p = s; *p && n < 24; p++)
        if (*p == ',') *p = 0, f[n++] = p + 1;
    const char* t = f[0] + 3;   // GP / GN / BD / GL
    if (!strcmp(t, "GSV") && n >= 4) {   // msg i of k: up to 4 satellites each
        int k = atoi(f[1]), i = atoi(f[2]);
        if (i == 1) gsvBuild = 0;
        for (int j = 4; j + 3 < n + 1 && gsvBuild < 24; j += 4) {
            if (j + 3 >= n) break;
            if (!*f[j]) continue;
            gsvTmp[gsvBuild++] = {(uint8_t)atoi(f[j]), (uint8_t)atoi(f[j + 3]), f[0][2]};
        }
        if (i == k) {   // the last one of a talker's set: publish (merge talkers by replacing theirs)
            char sys = f[0][2];
            uint8_t m = 0;
            Sat keep[24];
            for (uint8_t q = 0; q < nsats; q++) if (sats[q].sys != sys && m < 24) keep[m++] = sats[q];
            for (uint8_t q = 0; q < gsvBuild && m < 24; q++) keep[m++] = gsvTmp[q];
            memcpy(sats, keep, sizeof(Sat) * m);
            nsats = m;
        }
    } else if (!strcmp(t, "GGA") && n >= 10) {
        int q = atoi(f[6]);
        satsSeen = atoi(f[7]);
        pend.sats = satsSeen;
        pend.hdop = *f[8] ? atof(f[8]) : 99;
        pend.alt = atof(f[9]);
        if (q > 0) {
            pend.lat = coord(f[2], f[3]);
            pend.lon = coord(f[4], f[5]);
            if (!isnan(pend.lat) && !isnan(pend.lon)) {
                pend.acc = max(2.5f, pend.hdop * 5.0f);
                pend.valid = true;
                pend.ms = millis();
                float sp = fix.spd, cr = fix.crs;
                fix = pend;
                fix.spd = sp, fix.crs = cr;
            }
        }
    } else if (!strcmp(t, "RMC") && n >= 9 && *f[2] == 'A') {
        fix.spd = atof(f[7]) * 0.514444f;
        if (fix.spd > 0.8f && *f[8]) fix.crs = atof(f[8]);
    }
}

bool haveFix() { return fix.valid && millis() - fix.ms < 3000; }

// ---- screen: 128 x 128, redrawn into a sprite twice a second ----
uint16_t c565(uint32_t rgb) { return lgfx::color565(rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF); }

// Text that never runs past the border: trimmed (ending in "~") to what fits from x to 122.
void put(const char* str, int x, int y) {
    char t[48];
    strlcpy(t, str, sizeof t);
    size_t n = strlen(t);
    if (fb.textWidth(t) <= 122 - x) { fb.drawString(t, x, y); return; }
    while (n > 1) {
        t[--n] = 0;
        t[n - 1] = '~';
        if (fb.textWidth(t) <= 122 - x) break;
    }
    fb.drawString(t, x, y);
}

void frame(bool fx) {   // the whole display edged green when locked, red when not
    uint16_t c = fx ? c565(0x39ff88) : c565(0xff4d6d);
    fb.drawRect(0, 0, 128, 128, c);
    fb.drawRect(1, 1, 126, 126, c);
    fb.fillRect(0, 0, 128, 16, c);
    fb.setTextColor(c565(0x05070b));
    fb.setFont(&fonts::Font2);
    fb.setTextDatum(middle_center);
    fb.drawString(fx ? "GPS LOCKED" : "NO GPS LOCK", 64, 8);
    fb.setTextDatum(top_left);
}

void drawSats() {
    bool fx = haveFix();
    char b[40];
    fb.setFont(&fonts::Font0);
    fb.setTextColor(c565(0xdbe4ee));
    if (fx) {
        snprintf(b, sizeof b, "%.6f", fix.lat); put(b, 6, 20);
        snprintf(b, sizeof b, "%.6f", fix.lon); put(b, 6, 30);
        snprintf(b, sizeof b, "+/-%dm alt %dm", (int)lroundf(fix.acc), (int)lroundf(fix.alt)); put(b, 6, 40);
        snprintf(b, sizeof b, "%dkm/h HDOP %.1f", (int)lroundf(fix.spd * 3.6f), fix.hdop); put(b, 6, 50);
    } else {
        fb.setTextColor(nmeaGood ? c565(0xffb020) : c565(0xff4d6d));
        put(nmeaGood ? "searching the sky" : "GPS not talking", 6, 24);
        fb.setTextColor(c565(0x94a3b8));
        put(nmeaGood ? "needs open sky" : "check the base", 6, 36);
    }
    // signal bars, strongest first: green tracked hard, amber weak, grey not tracked
    Sat v[24];
    uint8_t m = nsats;
    memcpy(v, sats, sizeof(Sat) * m);
    for (uint8_t i = 1; i < m; i++)
        for (uint8_t j = i; j > 0 && v[j].snr > v[j - 1].snr; j--) { Sat t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    uint8_t shown = min<uint8_t>(m, 12);
    int bw = shown ? min(9, 116 / shown) : 9, base = 118;
    for (uint8_t i = 0; i < shown; i++) {
        int h = min(50, v[i].snr * 50 / 50);
        uint16_t c = v[i].snr >= 30 ? c565(0x39ff88) : v[i].snr >= 20 ? c565(0xffb020) : v[i].snr ? c565(0xff4d6d) : c565(0x334155);
        fb.fillRect(6 + i * bw, base - max(2, h), bw - 2, max(2, h), c);
    }
    fb.setTextColor(c565(0x64748b));
    snprintf(b, sizeof b, "%u in view, %u used", nsats, fix.sats);
    put(b, 6, 60);
}

void drawNode() {
    char b[40], id[7];
    meshSelfIdHex(id);
    fb.setFont(&fonts::Font2);
    fb.setTextColor(c565(0x2dd4bf));
    snprintf(b, sizeof b, "NODE %s", id);
    put(b, 6, 18);
    if (wdOn()) {   // the GPS wardriver: what it has found, one count per line
        fb.setTextColor(c565(0x22d3ee));
        snprintf(b, sizeof b, "AP  %lu", (unsigned long)wdWifiCount());
        put(b, 6, 34);
        fb.setTextColor(c565(0xff2bd6));
        snprintf(b, sizeof b, "BLE %lu", (unsigned long)wdBleCount());
        put(b, 6, 50);
    } else {   // the GPS node: a detector that knows where it is
        fb.setTextColor(c565(0x7dd3fc));
        put("DETECTOR+GPS", 6, 34);
    }
    fb.setFont(&fonts::Font0);
    uint8_t peers = meshNodeCount();
    fb.setTextColor(peers ? c565(0x2dd4bf) : c565(0x64748b));
    snprintf(b, sizeof b, "MESH %u node%s", peers, peers == 1 ? "" : "s");
    put(b, 6, 70);
    char ch[24];
    wdChannelsText(ch, sizeof ch);
    fb.setTextColor(c565(0x94a3b8));
    snprintf(b, sizeof b, "CH  %s", ch);
    put(b, 6, 82);
    snprintf(b, sizeof b, "UP  %lum", (unsigned long)(millis() / 60000));
    put(b, 6, 94);
    int x = (millis() / 20) % 120;   // a sweep: alive, clear of the hint below
    fb.drawFastHLine(4 + max(0, x - 20), 108, min(20, x), c565(0x2dd4bf));
}

void draw() {
    fb.fillScreen(c565(0x05070b));
    frame(haveFix());
    if (page == 0) drawSats();
    else drawNode();
    fb.setFont(&fonts::Font0);
    fb.setTextColor(c565(0x475569));
    fb.setTextDatum(bottom_right);
    fb.drawString(page == 0 ? "tap: node" : "tap: sats", 124, 126);
    fb.setTextDatum(top_left);
    fb.pushSprite(0, 0);
}

}  // namespace

bool gpsNodeFix(double& lat, double& lon, float& acc) {
    if (!haveFix()) return false;
    lat = fix.lat, lon = fix.lon, acc = fix.acc;
    return true;
}

void gpsNodeBegin() {
    pinMode(BTN, INPUT_PULLUP);
    lcd.init();
    lcd.setRotation(1);   // 90 degrees clockwise
    lcd.setBrightness(110);
    fb.setColorDepth(16);
    fb.createSprite(128, 128);
    Serial1.setRxBufferSize(1024);
    Serial1.begin(9600, SERIAL_8N1, GPS_RX, -1);
    {
        Preferences p;
        p.begin("gpsnode", true);
        bool d = p.getBool("dark", false);
        p.end();
        if (d) setDark(true);
    }
    if (!dark) draw();
}

void gpsNodeLoop() {
    while (Serial1.available()) {
        char c = Serial1.read();
        if (c == '\n' || c == '\r') {
            nmea[nlen] = 0;
            if (nlen > 6 && nmea_ok(nmea)) nmeaGood++, sentence(nmea);
            nlen = 0;
        } else if (nlen < (int)sizeof nmea - 1) {
            nmea[nlen++] = c;
        } else {
            nlen = 0;
        }
    }
    uint32_t now = millis();
    static uint32_t lastDraw = 0, lastPos = 0, lastLine = 0, tapAt = 0;
    static bool btnWas = false, single = false;
    // The screen is the button. Tap: flip pages. Double-tap: screen off / on (incognito),
    // in any mode. A tap waits 350 ms to be sure it is not the first of two.
    bool btn = digitalRead(BTN) == LOW;
    if (btn && !btnWas) {
        if (single && now - tapAt < 350) { single = false; setDark(!dark); lastDraw = 0; }
        else { single = true; tapAt = now; }
    }
    btnWas = btn;
    if (single && now - tapAt >= 350) { single = false; if (!dark) { page ^= 1; lastDraw = 0; } }
    if (!dark && now - lastDraw > 500) { lastDraw = now; draw(); }
    if (haveFix() && now - lastPos > 10000) {   // where we are, to the home and the maps
        lastPos = now;
        meshSendPos(fix.lat, fix.lon, fix.acc, fix.sats, fix.spd, fix.crs);
    }
    if (now - lastLine > 1000) {   // for a Tab5 (or anything) on our USB
        lastLine = now;
        if (haveFix())
            Serial.printf("GPS {\"lat\":%.7f,\"lon\":%.7f,\"acc\":%.1f,\"alt\":%.1f,\"sats\":%u,\"spd\":%.1f,\"crs\":%.1f}\n",
                          fix.lat, fix.lon, fix.acc, fix.alt, fix.sats, fix.spd, fix.crs);
        else
            Serial.printf("GPS {\"fix\":0,\"sats\":%u,\"talking\":%d}\n", satsSeen, nmeaGood ? 1 : 0);
    }
}

#else
void gpsNodeBegin() {}
void gpsNodeLoop() {}
bool gpsNodeFix(double&, double&, float&) { return false; }
#endif
