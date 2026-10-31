#include "meshlink.h"
#include "app.h"
#include "feed.h"
#include "gps.h"
#include "model.h"
#include "settings.h"
#include "wd.h"
#include <mbedtls/base64.h>

namespace {

constexpr uint32_t BAUD = 115200;
constexpr uint32_t GAP_MS = 20000;          // between our messages: LoRa airtime is shared
constexpr uint32_t POS_EVERY_MS = 300000;   // position at least this often...
constexpr double POS_MOVE_M = 100;          // ...or when it moved this far
constexpr uint32_t HELLO_MS = 300000;       // nothing else to say: hello, so home keeps its summaries coming
constexpr uint32_t WD_EVERY_MS = 300000;    // a wardriver's session counts
constexpr uint32_t SWAP_MS = 180000;        // nothing heard: try the wires the other way round
constexpr int REC = 12, BATCH = 12;         // OSM2 B records (mtastic.py has the layout)
constexpr int DETQ = 60;
constexpr uint32_t DEDUP_MS = 60000;        // same device, same node, again this soon: not resent

struct LinkDet { uint8_t mac[6], node[3]; int8_t rssi; uint8_t flags; uint32_t ms; };
LinkDet detq[DETQ];
int ndet = 0;
struct Sent { uint32_t h, ms; } sentDet[64];
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
char state[64] = "off";
uint32_t heardMs = 0, sentMs = 0, posMs = 0, helloMs = 0, wdMs = 0, wdLast = 0;
double posLat = 0, posLon = 0;
int rxPin = 53, txPin = 54;   // a straight Grove cable: the C6L's TX (GPIO 5) lands on 53

const char *our_node() {
    const char *id = usb_node_id();
    return id && *id ? id : "TAB5";
}

bool hex_bytes(const char *s, uint8_t *out, int n) {   // "4B4CFC" or "AA:BB:..": n bytes
    int k = 0, hi = -1;
    for (; *s && k < n; s++) {
        int v = isdigit((unsigned char)*s) ? *s - '0' : isxdigit((unsigned char)*s) ? toupper(*s) - 'A' + 10 : -1;
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else out[k++] = hi << 4 | v, hi = -1;
    }
    return k == n;
}

void send(const char *text) {
    Serial1.print(text);   // TEXTMSG: what arrives in one burst goes out as one message
    Serial1.flush();
    sentMs = millis();
}

double dist_m(double la1, double lo1, double la2, double lo2) {
    double dx = (lo2 - lo1) * 111320.0 * cos(la1 * M_PI / 180), dy = (la2 - la1) * 111320.0;
    return sqrt(dx * dx + dy * dy);
}

// OSM1 S 6/7 12 AD5484:1:0,4B4CFC:1:-77,...
void on_summary(char *p) {
    char *nodes = strchr(p, ' ');
    if (nodes) nodes = strchr(nodes + 1, ' ');
    if (!nodes) return;
    for (char *tok = strtok(nodes + 1, ","); tok; tok = strtok(nullptr, ",")) {
        char id[7];
        int up = 0, rssi = 0;
        if (sscanf(tok, "%6[0-9A-Fa-f]:%d:%d", id, &up, &rssi) >= 2) feed_link_node(id, up != 0, rssi);
    }
}

// OSM2 B<base64>: the home's detections, shown as if our own nodes had heard them.
void on_batch(const char *b64) {
    uint8_t raw[REC * BATCH + 8];
    size_t n = 0;
    if (mbedtls_base64_decode(raw, sizeof raw, &n, (const uint8_t *)b64, strlen(b64))) return;
    for (size_t k = 0; k + REC <= n; k += REC) {
        const uint8_t *r = raw + k;
        char mac[18], node[7];
        snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", r[0], r[1], r[2], r[3], r[4], r[5]);
        snprintf(node, sizeof node, "%02X%02X%02X", r[6], r[7], r[8]);
        feed_link_det(node, mac, (int8_t)r[9], r[10] & 1, r[10] & 2, r[11]);
    }
}

void on_line(char *line) {
    // TEXTMSG prints "<sender short name>: <text>"; a line glitch can leave junk in front.
    char *t = strstr(line, ": ");
    t = t ? t + 2 : line;
    heardMs = millis();
    if (!strncmp(t, "OSM1 S ", 7)) on_summary(t + 7);
    else if (!strncmp(t, "OSM2 B", 6)) on_batch(t + 6);
    else if (!strncmp(t, "OUI-SPY", 7)) toast("%.90s", t);
    else if (!strncmp(t, "OSM", 3)) return;   // another Tab5's lines: not for us
    else toast("mesh: %.90s", t);   // a person typed it
}

// Our nodes' detections, up to 12 a message.
bool send_batch() {
    uint8_t raw[REC * BATCH];
    int n = 0;
    uint32_t now = millis();
    portENTER_CRITICAL(&mux);
    for (; n < BATCH && n < ndet; n++) {
        const LinkDet &d = detq[n];
        uint8_t *r = raw + n * REC;
        memcpy(r, d.mac, 6);
        memcpy(r + 6, d.node, 3);
        r[9] = (uint8_t)d.rssi;
        r[10] = d.flags;
        r[11] = (uint8_t)min<uint32_t>(255, (now - d.ms) / 1000);
    }
    memmove(detq, detq + n, sizeof(LinkDet) * (ndet - n));
    ndet -= n;
    portEXIT_CRITICAL(&mux);
    if (!n) return false;
    char out[210] = "OSM2 B";
    size_t olen = 0;
    if (mbedtls_base64_encode((uint8_t *)out + 6, sizeof out - 6, &olen, raw, n * REC)) return false;
    out[6 + olen] = 0;
    send(out);
    return true;
}

void tick() {
    uint32_t now = millis();
    // On Wi-Fi the dashboard feeds us: home needn't spend airtime on us. Say so at once,
    // and again when it changes, and keep only the hello going meanwhile.
    static int8_t wasSock = -1;
    bool sock = M->sockUp;
    if (sock != wasSock) wasSock = sock, helloMs = 0;
    if (sentMs && now - sentMs < GAP_MS) return;
    char out[200];
    if (sock) {
        if (!helloMs || now - helloMs > HELLO_MS) {
            snprintf(out, sizeof out, "OSM1 H %s wifi", our_node());
            helloMs = now;
            send(out);
        }
        return;
    }
    GpsFix f = gps_fix();
    if (f.valid && (!posMs || now - posMs > POS_EVERY_MS || dist_m(posLat, posLon, f.lat, f.lon) > POS_MOVE_M)) {
        snprintf(out, sizeof out, "OSM1 P %s %.5f %.5f %.0f", our_node(), f.lat, f.lon, f.acc);
        posMs = now, posLat = f.lat, posLon = f.lon, helloMs = now;
        send(out);
        return;
    }
    if (send_batch()) {
        helloMs = now;
        return;
    }
    uint32_t wdNow = wd_session_count(0);
    if (wdNow && wdNow != wdLast && (!wdMs || now - wdMs > WD_EVERY_MS)) {
        snprintf(out, sizeof out, "OSM1 W %s %lu %lu", our_node(), (unsigned long)wd_session_count(1),
                 (unsigned long)wd_session_count(2));
        wdMs = now, wdLast = wdNow, helloMs = now;
        send(out);
        return;
    }
    if (!helloMs || now - helloMs > HELLO_MS) {
        snprintf(out, sizeof out, "OSM1 H %s lora", our_node());
        helloMs = now;
        send(out);
    }
}

}  // namespace

void meshlink_run() {
    uint32_t openedMs = millis();
    Serial1.setRxBufferSize(1024);
    Serial1.begin(BAUD, SERIAL_8N1, rxPin, txPin);
    char line[256];
    int len = 0;
    while (S.meshLink) {
        while (Serial1.available()) {
            char c = Serial1.read();
            if (c == '\n' || c == '\r') {
                line[len] = 0;
                if (len) on_line(line);
                len = 0;
            } else if (len < (int)sizeof line - 1 && (uint8_t)c >= 0x20) {
                line[len++] = c;
            }
        }
        tick();
        uint32_t now = millis();
        int q;
        portENTER_CRITICAL(&mux);
        q = ndet;
        portEXIT_CRITICAL(&mux);
        if (heardMs) {
            snprintf(state, sizeof state, "mesh link: heard home %lus ago, %d to send", (unsigned long)((now - heardMs) / 1000), q);
        } else {
            snprintf(state, sizeof state, "mesh link: waiting for home (port A %d/%d)", rxPin, txPin);
            if (now - openedMs > SWAP_MS) {   // the wires may be the other way round
                std::swap(rxPin, txPin);
                Serial1.end();
                Serial1.begin(BAUD, SERIAL_8N1, rxPin, txPin);
                openedMs = now;
                helloMs = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    Serial1.end();
    strlcpy(state, "off", sizeof state);
}

void meshlink_det(const char *node, const char *mac, int rssi, bool wifi, bool isNew) {
    if (!S.meshLink) return;
    LinkDet d;
    if (!hex_bytes(mac, d.mac, 6) || !hex_bytes(node, d.node, 3)) return;
    d.rssi = constrain(rssi, -128, 127);
    d.flags = (wifi ? 1 : 0) | (isNew ? 2 : 0);
    d.ms = millis();
    uint32_t h = 2166136261u;
    for (uint8_t b : d.mac) h = (h ^ b) * 16777619u;
    for (uint8_t b : d.node) h = (h ^ b) * 16777619u;
    portENTER_CRITICAL(&mux);
    int slot = 0;
    bool dup = false;
    for (int i = 0; i < 64; i++) {
        if (sentDet[i].h == h && d.ms - sentDet[i].ms < DEDUP_MS) { dup = true; break; }
        if (sentDet[i].ms < sentDet[slot].ms) slot = i;
    }
    if (!dup) {
        sentDet[slot] = {h, d.ms};
        if (ndet == DETQ) memmove(detq, detq + 1, sizeof(LinkDet) * --ndet);   // full: the oldest goes
        detq[ndet++] = d;
    }
    portEXIT_CRITICAL(&mux);
}

const char *meshlink_state() { return state; }
