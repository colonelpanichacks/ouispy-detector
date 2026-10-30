#include "gps.h"
#include <M5Unified.h>

namespace {

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
GpsFix fix = {};
bool present = false;
uint32_t baudNow = 0;
uint8_t satsSeen = 0;
char state[48] = "no GPS";
float trail[GPS_TRAIL][2];
int ntrail = 0;

// NMEA: "$..*hh" with a matching XOR checksum.
bool nmea_ok(const char *s) {
    if (s[0] != '$') return false;
    const char *star = strchr(s, '*');
    if (!star || !isxdigit((unsigned char)star[1]) || !isxdigit((unsigned char)star[2])) return false;
    uint8_t x = 0;
    for (const char *p = s + 1; p < star; p++) x ^= (uint8_t)*p;
    return x == (uint8_t)strtol(star + 1, nullptr, 16);
}

// Split into fields in place; returns the count.
int fields(char *s, char **f, int max) {
    int n = 0;
    char *star = strchr(s, '*');
    if (star) *star = 0;
    f[n++] = s;
    for (char *p = s; *p && n < max; p++)
        if (*p == ',') *p = 0, f[n++] = p + 1;
    return n;
}

double coord(const char *v, const char *hemi) {   // ddmm.mmmm -> degrees
    if (!*v) return NAN;
    double x = atof(v);
    int deg = (int)(x / 100);
    double d = deg + (x - deg * 100) / 60.0;
    if (*hemi == 'S' || *hemi == 'W') d = -d;
    return d;
}

void add_trail(double la, double lo) {
    if (ntrail) {
        double dy = (la - trail[ntrail - 1][0]) * 111320.0;
        double dx = (lo - trail[ntrail - 1][1]) * 111320.0 * cos(la * M_PI / 180);
        if (dx * dx + dy * dy < 9) return;   // < 3 m
    }
    if (ntrail == GPS_TRAIL) memmove(trail[0], trail[1], sizeof trail[0] * (GPS_TRAIL - 1)), ntrail--;
    trail[ntrail][0] = la, trail[ntrail][1] = lo;
    ntrail++;
}

GpsFix pending = {};   // GGA and RMC of one second land here

void sentence(char *s) {
    char *f[24];
    int n = fields(s, f, 24);
    const char *t = f[0] + 3;   // talker-independent: GP / GN / BD / GL
    if (!strcmp(t, "GGA") && n >= 10) {
        int q = atoi(f[6]);
        satsSeen = atoi(f[7]);
        pending.sats = satsSeen;
        pending.hdop = *f[8] ? atof(f[8]) : 99;
        pending.alt = atof(f[9]);
        if (q > 0) {
            pending.lat = coord(f[2], f[3]);
            pending.lon = coord(f[4], f[5]);
        }
        if (q > 0 && !isnan(pending.lat) && !isnan(pending.lon)) {
            pending.acc = max(2.5f, pending.hdop * 5.0f);
            pending.valid = true;
            pending.ms = millis();
            portENTER_CRITICAL(&mux);
            float sp = fix.speed, co = fix.course;
            fix = pending;
            fix.speed = sp, fix.course = co;
            add_trail(fix.lat, fix.lon);
            portEXIT_CRITICAL(&mux);
        }
    } else if (!strcmp(t, "RMC") && n >= 9) {
        if (*f[2] == 'A') {
            float sp = atof(f[7]) * 0.514444f, co = atof(f[8]);
            portENTER_CRITICAL(&mux);
            fix.speed = sp;
            if (sp > 0.8f && *f[8]) fix.course = co;
            portEXIT_CRITICAL(&mux);
        }
    }
}

// Read for up to `ms` at this baud: true once two valid sentences arrive. The UART is
// opened once per pin pair (open()); only the baud changes here, so a Tab5 with no GPS
// doesn't churn the UART driver (and internal RAM) every few seconds.
int openRx = -1;
void open_pins(int rx, int tx, uint32_t baud) {
    if (openRx == rx) { Serial1.updateBaudRate(baud); return; }
    if (openRx >= 0) Serial1.end();
    Serial1.setRxBufferSize(1024);
    Serial1.begin(baud, SERIAL_8N1, rx, tx);
    openRx = rx;
}

bool probe(int rx, int tx, uint32_t baud, uint32_t ms) {
    open_pins(rx, tx, baud);
    while (Serial1.available()) Serial1.read();
    char line[100];
    int len = 0, good = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < ms) {
        while (Serial1.available()) {
            char c = Serial1.read();
            if (c == '\n' || c == '\r') {
                line[len] = 0;
                if (len > 6 && nmea_ok(line) && ++good >= 2) return true;
                len = 0;
            } else if (len < (int)sizeof line - 1) {
                line[len++] = c;
            } else {
                len = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

void gps_task(void *) {
    static const int pins[2][2] = {{53, 54}, {54, 53}};   // (rx, tx): units differ in which wire is TX
    static const uint32_t bauds[] = {9600, 115200, 38400, 57600, 4800};
    for (;;) {
        // Search.
        present = false;
        int rx = -1;
        while (rx < 0) {
            for (auto &p : pins) {
                for (uint32_t b : bauds) {
                    snprintf(state, sizeof state, "no GPS (looking %lu)", (unsigned long)b);
                    if (probe(p[0], p[1], b, 1300)) { rx = p[0], baudNow = b; break; }
                }
                if (rx >= 0) break;
            }
            if (rx < 0) {   // nothing on the port: look again in a while (plugging one in is noticed then)
                snprintf(state, sizeof state, "no GPS");
                vTaskDelay(pdMS_TO_TICKS(30000));
            }
        }
        present = true;
        // Read until it goes quiet for 5 s.
        char line[120];
        int len = 0;
        uint32_t lastGood = millis();
        while (millis() - lastGood < 5000) {
            while (Serial1.available()) {
                char c = Serial1.read();
                if (c == '\n' || c == '\r') {
                    line[len] = 0;
                    if (len > 6 && nmea_ok(line)) {
                        lastGood = millis();
                        sentence(line);
                    }
                    len = 0;
                } else if (len < (int)sizeof line - 1) {
                    line[len++] = c;
                } else {
                    len = 0;
                }
            }
            GpsFix f = gps_fix();
            if (f.valid) snprintf(state, sizeof state, "GPS fix  %u sats  +/-%d m", f.sats, (int)lroundf(f.acc));
            else snprintf(state, sizeof state, "GPS (%lu): %u sats, no fix yet", (unsigned long)baudNow, satsSeen);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

}  // namespace

void gps_begin() {
    M5.Power.setExtOutput(true);   // 5 V out on the ports: the unit's power
    xTaskCreatePinnedToCore(gps_task, "gps", 4096, nullptr, 2, nullptr, 0);
}

void gps_external(double lat, double lon, float acc, float alt, uint8_t sats, float spd, float crs) {
    if (present) return;   // our own module wins
    portENTER_CRITICAL(&mux);
    fix.valid = true;
    fix.lat = lat, fix.lon = lon, fix.acc = acc, fix.alt = alt, fix.sats = sats, fix.speed = spd, fix.course = crs;
    fix.ms = millis();
    add_trail(lat, lon);
    portEXIT_CRITICAL(&mux);
    snprintf(state, sizeof state, "GPS from the node on USB  %u sats", sats);
}

GpsFix gps_fix() {
    portENTER_CRITICAL(&mux);
    GpsFix f = fix;
    portEXIT_CRITICAL(&mux);
    if (f.valid && millis() - f.ms > 3000) f.valid = false;
    return f;
}

const char *gps_state() { return state; }
bool gps_present() { return present; }

int gps_trail(float (*out)[2], int max) {
    portENTER_CRITICAL(&mux);
    int n = min(ntrail, max), s = ntrail - n;
    memcpy(out, trail[s], sizeof trail[0] * n);
    portEXIT_CRITICAL(&mux);
    return n;
}
