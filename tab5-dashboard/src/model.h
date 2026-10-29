// Live copy of the OUI-SPY Mesh dashboard's state, fed by net.cpp (Socket.IO events and
// HTTP polls) and read by ui.cpp. Everything here is guarded by model_lock().
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

constexpr int MAX_NODES = 16;
constexpr int MAX_DETS = 2000;     // ring of the newest detections (also the USB-mode history)
constexpr int MAX_TARGETS = 64;    // devices heard recently, with who heard them
constexpr int MAX_SEEN = 6;        // nodes kept per seen_by list
constexpr int MAX_BARS = 24;       // vendor / node / type rows kept from /api/metrics
constexpr int MAX_BUCKETS = 192;
constexpr int MAX_MUTED = 32;
constexpr int MAX_DRONES = 16;
constexpr int DRONE_TRACK = 48;    // flight track points kept per drone
constexpr float DRONE_LIVE_S = 90;

constexpr float TARGET_LIVE_S = 60;     // server SEEN_WINDOW: "heard lately"
constexpr float TARGET_KEEP_S = 600;    // drop a target this long after it was last heard
constexpr float ALERT_COOLDOWN_S = 600; // same MAC again within this = no new alert

struct Seen {
    char node[7];
    int16_t rssi;
    uint16_t age;   // s, as of the event that carried the list
};

struct Node {
    char id[7];
    char alias[24];
    char role[8];
    char mode[8];
    char fw[20];
    char fcrc[9];
    char ota[10];          // ota.state ("", "running", "done", "error"...)
    int8_t otaPct;
    bool online, isHome;
    int8_t filtersFresh;   // 1 list matches fcrc, 0 stale, -1 unknown
    int16_t rssi;          // last link RSSI the home measured (0 = n/a)
    int16_t lrssi;         // node's own strongest neighbour RSSI
    int16_t filters;       // filter count the node reports (-1 = n/a)
    int16_t peers, ch, dev;
    int16_t hue;           // dashboard colour (hsl hue), -1 = none
    int8_t buzzer, led;    // its audio settings from the status (-1 = not known yet)
    uint32_t up, heap;
    float age;             // s since the server last heard it
    double since;
    double lat, lon;       // map position (dashboard), hasPos when set
    bool hasPos;
    uint8_t nnbr;
    Seen nbr[MAX_SEEN];    // its mesh neighbours (age field holds the hop flag)
};

struct Home {
    char id[7];
    char source[10];
    char rate[10];
    bool alive, legacy;
    int enc, mesh, ch;
    uint32_t rx, dup, tx, txf, pend, bad, up, heap;
    int lrssi;
    float age;
};

struct Det {
    uint32_t id;
    double ts;
    char node[7];
    char mac[18];
    char descr[36];
    char alias[24];
    char type[8];
    char dom[5];
    char closest[7];
    char name[33];         // what the device calls itself (BLE name / AP SSID)
    char group[24];        // dashboard group alias
    int16_t rssi;
    bool isNew;
    uint8_t nseen;
    Seen seen[MAX_SEEN];
};

struct Target {
    char mac[18];
    char name[33];
    char alias[24];
    char group[24];
    char info[360];       // latest over-the-air intel JSON (USB mode detail)
    // Triangulated position from the dashboard (tri.py, same model as its web map).
    bool triOk, triRough;
    double triLat, triLon;
    float triErr;          // m, 68% radius
    double triTs;          // server time of the fix
    char descr[36];
    char type[8];
    char dom[5];
    double first, last;   // server time
    uint32_t hits;
    uint8_t nseen;
    Seen seen[MAX_SEEN];  // strongest first, age as of `last`
};

struct Drone {
    char mac[18];
    char alias[24];
    char id[24];          // Remote ID serial / registration
    char op[24];          // operator ID
    char desc[24];
    char ua[28];          // aircraft type
    char status[20];
    char tag[12];
    char src[8];
    char closest[7];
    char vendor[32];      // its radio's maker (MAC OUI)
    char maker[48];       // FAA registry: make + model (from the serial)
    double lat, lon, plat, plon;
    float alt, hagl, spd, vspd, hdg;
    bool hasPos, hasPilot, hasAlt, hasHagl, hasSpd, hasHdg;
    int16_t rssi;
    int32_t pilotDist;    // m, -1 unknown
    double first, last;   // server time
    uint32_t reports;
    uint8_t ntrack;
    float track[DRONE_TRACK][2];
};

// One device's full record, fetched when its detail page opens.
struct DevDetail {
    char mac[18];
    bool loading, valid;
    char name[33], alias[24], group[24];
    int nrows;
    char rows[30][2][120];
    int ncomp;
    struct { char mac[18]; char label[36]; char group[24]; uint16_t together, occasions; } comp[8];
    int ngroups;
    struct { int id; char name[24]; int n; } groups[16];
};

struct Bar {
    char name[28];
    uint32_t count, devices;
};

struct Metrics {
    bool valid;
    uint32_t range;       // s the numbers cover
    uint32_t bucket;      // s per series point
    double start, until;
    uint32_t total, prevTotal, unique, newDevices, nodesActive, n;
    uint32_t peak;
    double peakTs;
    uint16_t nbuckets;
    uint16_t series[MAX_BUCKETS];    // all vendors summed
    uint16_t uniqueSeries[MAX_BUCKETS];
    uint8_t nvendor, nnode, ntype;
    Bar vendor[MAX_BARS], node[MAX_BARS], type[MAX_BARS];
    uint32_t ble, wifi;
    int rssiFrom, rssiStep;
    uint8_t nrssi;
    uint16_t rssi[24];
    uint32_t fetchedMs;
};

struct Stats {
    uint32_t day, total, macs;
    uint16_t hourly[24];
    uint8_t nhourly;
};

struct Model {
    // link
    bool wifiUp, sockUp, stateLoaded;
    bool usbLive;            // a node on USB-A is talking
    bool bridging;           // ...and its lines go up to the dashboard, which feeds us back
    bool clockValid;         // wall time known (dashboard or RTC), not just uptime
    char mode[8];            // live / demo / replay
    char lastError[64];
    uint32_t events;         // Socket.IO events received
    uint32_t lastEventMs;
    // server clock: serverTs at serverMs (millis)
    double serverTs;
    uint32_t serverMs;

    Home home;
    int nnodes;
    Node nodes[MAX_NODES];

    int ndets;               // valid entries in the ring
    int detHead;             // index of the newest
    Det dets[MAX_DETS];
    uint32_t maxDetId;

    int ntargets;
    Target targets[MAX_TARGETS];

    Stats stats;
    Metrics metrics;

    int ndrones;
    Drone drones[MAX_DRONES];
    uint32_t droneSeq;       // bumps when a drone first shows up

    DevDetail detail;

    // alerts
    uint32_t alertsUnacked;
    Det lastAlert;
    bool hasAlert;
    bool muteAll;
    int nmuted;
    char muted[MAX_MUTED][18];
    uint32_t alertSeq;       // bumps on every new alert (ui chirps / flashes on change)

    uint32_t version;        // bumps on every change; ui redraws when it moves
};

extern Model *M;   // lives in PSRAM

void model_init();
void model_lock();
void model_unlock();

double server_now();                  // server epoch now, extrapolated
const Node *node_by_id(const char *id);
const char *node_name(const char *id);  // alias, else id
uint16_t node_color565(const char *id);
const Det *det_at(int i);             // 0 = newest
bool mac_muted(const char *mac);
void mute_toggle(const char *mac);
void alerts_ack();

// JSON ingestion (caller holds no lock; these take it)
void ingest_state(JsonDocument &doc);
void ingest_fleet(JsonObjectConst fleet);
void ingest_det(JsonObjectConst d, bool live);
void ingest_sighting(JsonObjectConst s);
void ingest_metrics(JsonDocument &doc, uint32_t range);
void ingest_stats(JsonDocument &doc);
void ingest_cleared(JsonObjectConst e);
void ingest_drone(JsonObjectConst d);          // dashboard "drone" event shape
void ingest_tri(JsonObjectConst f);            // dashboard "tri" event: a triangulated fix
void ingest_rid(JsonObjectConst r, const char *homeId);   // a node's raw "RID {...}" line (USB mode)
void ingest_detail(JsonDocument &doc);
void ingest_names(JsonObjectConst aliases, JsonObjectConst names, JsonObjectConst groupOf);
int live_drones();
void prune_targets();
// No-network mode: stats + metrics from the local history, same shapes as the dashboard's.
void model_local_stats(uint32_t range, const char *node, uint8_t dom);
void model_set_clock(double ts, bool valid);
