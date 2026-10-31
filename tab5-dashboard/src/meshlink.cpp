#include "meshlink.h"
#include "app.h"
#include "feed.h"
#include "gps.h"
#include "model.h"
#include "settings.h"

namespace {

constexpr uint32_t BAUD = 115200;
constexpr uint32_t GAP_MS = 20000;          // between our messages: LoRa airtime is shared
constexpr uint32_t POS_EVERY_MS = 300000;   // position at least this often...
constexpr double POS_MOVE_M = 100;          // ...or when it moved this far
constexpr uint32_t HELLO_MS = 300000;       // no fix: say hello so home starts its summaries
constexpr uint32_t SWAP_MS = 180000;        // nothing heard: try the wires the other way round
constexpr int DETQ = 8;

struct LinkDet { char node[7], mac[18], dom[5], name[24]; int16_t rssi; };
LinkDet detq[DETQ];
int ndet = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
char state[64] = "off";
uint32_t heardMs = 0, sentMs = 0, posMs = 0, helloMs = 0;
double posLat = 0, posLon = 0;
int rxPin = 53, txPin = 54;   // a straight Grove cable: the C6L's TX (GPIO 5) lands on 53

const char *our_node() {
    const char *id = usb_node_id();
    return id && *id ? id : "TAB5";
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

void on_line(char *line) {
    // TEXTMSG prints "<sender short name>: <text>".
    char *t = strstr(line, ": ");
    t = t ? t + 2 : line;
    heardMs = millis();
    if (!strncmp(t, "OSM1 S ", 7)) on_summary(t + 7);
    else if (!strncmp(t, "OUI-SPY", 7)) toast("%.90s", t);
    else if (!strncmp(t, "OSM1 ", 5)) return;   // another Tab5's lines: not for us
    else toast("mesh: %.90s", t);   // a person typed it
}

void tick() {
    uint32_t now = millis();
    if (sentMs && now - sentMs < GAP_MS) return;
    char out[200];
    GpsFix f = gps_fix();
    if (f.valid && (!posMs || now - posMs > POS_EVERY_MS || dist_m(posLat, posLon, f.lat, f.lon) > POS_MOVE_M)) {
        snprintf(out, sizeof out, "OSM1 P %s %.5f %.5f %.0f", our_node(), f.lat, f.lon, f.acc);
        posMs = now, posLat = f.lat, posLon = f.lon, helloMs = now;
        send(out);
        return;
    }
    LinkDet d;
    bool have = false;
    portENTER_CRITICAL(&mux);
    if (ndet) {
        d = detq[0];
        memmove(detq, detq + 1, sizeof(LinkDet) * --ndet);
        have = true;
    }
    portEXIT_CRITICAL(&mux);
    if (have) {
        snprintf(out, sizeof out, "OSM1 D %s %s %d %s %s", d.node, d.mac, d.rssi, d.dom, d.name);
        send(out);
        return;
    }
    if (!helloMs || now - helloMs > HELLO_MS) {
        snprintf(out, sizeof out, "OSM1 H %s", our_node());
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
        if (heardMs) {
            snprintf(state, sizeof state, "mesh link: heard home %lu s ago", (unsigned long)((now - heardMs) / 1000));
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

void meshlink_det(const char *node, const char *mac, int rssi, const char *dom, const char *name) {
    if (!S.meshLink) return;
    portENTER_CRITICAL(&mux);
    if (ndet < DETQ) {
        LinkDet &d = detq[ndet++];
        strlcpy(d.node, node, sizeof d.node);
        strlcpy(d.mac, mac, sizeof d.mac);
        strlcpy(d.dom, dom, sizeof d.dom);
        strlcpy(d.name, name, sizeof d.name);
        for (char *p = d.name; *p; p++)
            if (*p == '\n' || *p == '\r') *p = ' ';
        d.rssi = rssi;
    }
    portEXIT_CRITICAL(&mux);
}

const char *meshlink_state() { return state; }
