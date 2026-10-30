// Mirrors home-dashboard/fleet.py for one USB-attached node: roster from TLM, settings from
// RST, detections with seen_by / closest / dedup, signal updates as sightings.
#include "feed.h"
#include "app.h"
#include "model.h"
#include "settings.h"
#include "usbhost.h"
#include "net.h"
#include "gps.h"
#include "wd.h"

namespace {

constexpr int MAXN = MAX_NODES;
constexpr float HOME_TIMEOUT = 15, SEEN_WINDOW = 60, DEDUP_WINDOW = 6;
constexpr int SEEN_MACS = 128, DEDUP = 64, KNOWN = 4096;

struct SpiRamAllocator : ArduinoJson::Allocator {
    void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void deallocate(void *p) override { heap_caps_free(p); }
    void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
SpiRamAllocator psram;

struct LNode {
    char id[7];
    char role[8];
    bool online;
    int rssi;
    uint32_t pkts;
    double lastSeen, since;
    String cfg;   // last RST line body (JSON)
};
LNode nodes[MAXN];
int nn = 0;
char homeId[7] = "";    // the mesh's home node (role "home"), wherever it is
char localId[7] = "";   // the node on our USB port: its TLM is our link view. On a
                        // remote, the whole mesh still shows: every node prints the
                        // status every other node floods, and relayed detections.
char localRole[8] = "";
String tlm;          // last TLM body (JSON)
double tlmTs = 0;
volatile uint32_t lastLineMs = 0;

struct SeenMac {
    uint32_t h;
    char node[MAX_SEEN][7];
    int16_t rssi[MAX_SEEN];
    double ts[MAX_SEEN];
};
SeenMac *seen;      // PSRAM
struct Dedup {
    uint32_t h;
    double ts;
} dedup[DEDUP];
uint32_t *known;     // PSRAM open-addressing set of MAC hashes
uint32_t nextId = 0x80000000u;   // above any dashboard row id, so the two never collide

uint32_t fnv(const char *s, const char *t = nullptr) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    for (; t && *t; t++) h = (h ^ (uint8_t)*t) * 16777619u;
    return h ? h : 1;
}

bool known_add(const char *mac) {   // true if it was new
    uint32_t h = fnv(mac);
    for (uint32_t i = 0; i < KNOWN; i++) {
        uint32_t &slot = known[(h + i) & (KNOWN - 1)];
        if (slot == h) return false;
        if (!slot) {
            slot = h;
            return true;
        }
    }
    return false;   // full: stop calling things new
}

double now_s() { return server_now(); }

// The local view drives the model only when the dashboard isn't feeding us: with the
// socket up, the node's lines go up the bridge and come back merged with history.
bool local() { return !M->sockUp; }

LNode *node_get(const char *id, bool create) {
    for (int i = 0; i < nn; i++)
        if (!strcmp(nodes[i].id, id)) return &nodes[i];
    if (!create || nn == MAXN) return nullptr;
    LNode &n = nodes[nn++];
    n = LNode();
    strlcpy(n.id, id, sizeof n.id);
    strlcpy(n.role, "remote", sizeof n.role);
    return &n;
}

void node_seen(const char *id, const char *role, double now) {
    LNode *n = node_get(id, true);
    if (!n) return;
    if (role && (!strcmp(role, "home") || !strcmp(role, "remote"))) strlcpy(n->role, role, sizeof n->role);
    if (!n->online) n->since = now;
    n->online = true;
    n->lastSeen = max(n->lastSeen, now);
}

// Build the dashboard's fleet snapshot and hand it to the model.
void publish_fleet() {
    double now = now_s();
    JsonDocument doc(&psram);
    JsonObject home = doc["home"].to<JsonObject>();
    if (tlm.length()) {
        JsonDocument t(&psram);
        if (!deserializeJson(t, tlm))
            for (JsonPair kv : t.as<JsonObject>())
                if (strcmp(kv.key().c_str(), "nodes") && strcmp(kv.key().c_str(), "strays")) home[kv.key()] = kv.value();
    }
    home["id"] = localId;   // link counters are the local node's own
    home["alive"] = tlmTs && now - tlmTs < HOME_TIMEOUT;
    home["age"] = tlmTs ? now - tlmTs : -1;
    home["source"] = "usb";
    // The home's filter CRC is the reference: a remote holding the same list is in sync.
    String homeCrc;
    for (int i = 0; i < nn; i++)
        if (homeId[0] && !strcmp(nodes[i].id, homeId) && nodes[i].cfg.length()) {
            JsonDocument c(&psram);
            if (!deserializeJson(c, nodes[i].cfg)) homeCrc = (const char *)(c["fcrc"] | "");
        }
    JsonArray arr = doc["nodes"].to<JsonArray>();
    // Home first, then online, then by id: same order as the dashboard.
    int order[MAXN];
    for (int i = 0; i < nn; i++) order[i] = i;
    std::sort(order, order + nn, [](int a, int b) {
        bool ha = homeId[0] && !strcmp(nodes[a].id, homeId), hb = homeId[0] && !strcmp(nodes[b].id, homeId);
        if (ha != hb) return ha;
        if (nodes[a].online != nodes[b].online) return nodes[a].online;
        return strcmp(nodes[a].id, nodes[b].id) < 0;
    });
    for (int k = 0; k < nn; k++) {
        LNode &n = nodes[order[k]];
        JsonObject o = arr.add<JsonObject>();
        bool isHome = homeId[0] && !strcmp(n.id, homeId);
        bool isLocal = !strcmp(n.id, localId);
        o["id"] = n.id;
        o["role"] = isHome ? "home" : n.role;
        o["online"] = isLocal ? (bool)home["alive"] : n.online;
        if (isLocal && tlm.length() && strstr(tlm.c_str(), "\"kind\":\"wardrive\"")) {
            JsonDocument t(&psram);   // our own USB node is a wardriver: its switches for the editor
            if (!deserializeJson(t, tlm)) {
                JsonObject w = o["tlm"].to<JsonObject>();
                w["wdon"] = t["wdon"] | 0;
                w["wdch"] = t["wdch"] | "";
                w["wd"] = t["wd"];
                w["kind"] = "wardrive";
            }
        }
        o["is_home"] = isHome;
        if (n.rssi) o["rssi"] = n.rssi;
        o["age"] = n.lastSeen ? now - n.lastSeen : -1;
        o["since"] = n.since;
        if (n.cfg.length()) {
            JsonDocument c(&psram);
            if (!deserializeJson(c, n.cfg)) {
                o["cfg"] = c.as<JsonObject>();
                o["alias"] = c["alias"] | "";
                const char *crc = c["fcrc"] | "";
                if (homeCrc.length() && *crc) o["filters_fresh"] = homeCrc == crc;
            }
        }
    }
    doc["ts"] = now;
    if (local()) ingest_fleet(doc.as<JsonObjectConst>());
}

void on_tlm(const char *body) {
    JsonDocument t(&psram);
    if (deserializeJson(t, body)) return;
    double now = now_s();
    const char *id = t["id"] | "";
    const char *role = t["role"] | "";
    if (*id) strlcpy(localId, id, sizeof localId);
    strlcpy(localRole, role, sizeof localRole);
    if (!strcmp(role, "home")) strlcpy(homeId, localId, sizeof homeId);
    tlm = body;
    tlmTs = now;
    node_seen(localId, *role ? role : "remote", now);
    bool inRoster[MAXN] = {};
    for (JsonObjectConst r : t["nodes"].as<JsonArrayConst>()) {
        const char *nid = r["id"] | "";
        if (strlen(nid) != 6) continue;
        const char *rr = r["role"] | "";
        if (!strcmp(rr, "home")) strlcpy(homeId, nid, sizeof homeId);   // plugged into a remote: the roster names the home
        node_seen(nid, rr, now - (r["last"] | 0));
        LNode *n = node_get(nid, false);
        if (!n) continue;
        n->pkts = r["pkts"] | 0;
        int rs = r["rssi"] | 0;
        if (rs) n->rssi = rs;
        inRoster[n - nodes] = true;
    }
    // TLM is authoritative: anyone not in the roster has dropped.
    for (int i = 0; i < nn; i++)
        if (!inRoster[i] && strcmp(nodes[i].id, localId)) nodes[i].online = false;
    publish_fleet();
}

void on_rst(const char *body) {
    JsonDocument c(&psram);
    if (deserializeJson(c, body)) return;
    const char *nid = c["node"] | "";
    if (strlen(nid) != 6) return;
    LNode *n = node_get(nid, true);
    if (!n) return;
    n->cfg = body;
    const char *role = c["role"] | "";
    if (!strcmp(role, "home")) strlcpy(homeId, nid, sizeof homeId);
    if (strcmp(nid, localId)) node_seen(nid, role, now_s());
    publish_fleet();
}

// seen_by for `mac`: who heard it within SEEN_WINDOW, strongest first.
void sighting(JsonArray out, const char *mac, const char *node, int rssi, double now) {
    uint32_t h = fnv(mac);
    SeenMac *s = nullptr, *oldest = &seen[0];
    for (int i = 0; i < SEEN_MACS; i++) {
        if (seen[i].h == h) s = &seen[i];
        double newest = 0;
        for (int k = 0; k < MAX_SEEN; k++) newest = max(newest, seen[i].ts[k]);
        double oldestNewest = 0;
        for (int k = 0; k < MAX_SEEN; k++) oldestNewest = max(oldestNewest, oldest->ts[k]);
        if (newest < oldestNewest) oldest = &seen[i];
    }
    if (!s) {
        s = oldest;
        memset(s, 0, sizeof *s);
        s->h = h;
    }
    if (rssi) {
        int slot = -1, stale = 0;
        for (int k = 0; k < MAX_SEEN; k++) {
            if (!strcmp(s->node[k], node)) slot = k;
            if (s->ts[k] < s->ts[stale]) stale = k;
        }
        if (slot < 0) slot = stale;
        strlcpy(s->node[slot], node, 7);
        s->rssi[slot] = rssi;
        s->ts[slot] = now;
    }
    int idx[MAX_SEEN], n = 0;
    for (int k = 0; k < MAX_SEEN; k++)
        if (s->node[k][0] && now - s->ts[k] <= SEEN_WINDOW) idx[n++] = k;
    std::sort(idx, idx + n, [&](int a, int b) { return s->rssi[a] > s->rssi[b]; });
    for (int i = 0; i < n; i++) {
        JsonArray row = out.add<JsonArray>();
        row.add(s->node[idx[i]]);
        row.add(s->rssi[idx[i]]);
        row.add((int)(now - s->ts[idx[i]]));
    }
}

void on_detection(const char *body) {
    JsonDocument d(&psram);
    if (deserializeJson(d, body)) return;
    double now = now_s();
    String mac = (const char *)(d["mac"] | "");
    mac.toUpperCase();
    if (!mac.length()) return;
    String descr = (const char *)(d["desc"] | "");
    String node = (const char *)(d["node"] | "");
    int tag = descr.indexOf("[mesh ");   // older firmware tags relayed detections in desc
    if (tag >= 0) {
        if (!node.length()) node = descr.substring(tag + 6, tag + 12);
        descr = descr.substring(0, tag);
    }
    descr.trim();
    if (!node.length()) node = localId[0] ? localId : "HOME";   // no tag: our own node heard it
    node.toUpperCase();
    const char *type = d["type"] | "";
    String dom = (const char *)(d["dom"] | "");
    if (dom != "ble" && dom != "wifi") dom = (!strcasecmp(type, "PROBE") || !strcasecmp(type, "BEACON")) ? "wifi" : "ble";
    int rssi = d["rssi"] | 0;

    JsonDocument ev(&psram);
    JsonArray seenBy = ev["seen_by"].to<JsonArray>();
    sighting(seenBy, mac.c_str(), node.c_str(), rssi, now);
    if (LNode *n = node_get(node.c_str(), true)) {
        if (!n->online) n->since = now;
        n->online = true;
        n->lastSeen = now;
    }
    ev["ts"] = now;
    ev["node"] = node;
    ev["mac"] = mac;
    ev["rssi"] = rssi;
    ev["type"] = type;
    ev["match"] = d["match"];
    ev["descr"] = descr;
    ev["dom"] = dom;
    if (d["info"].is<JsonObject>()) ev["info"] = d["info"];   // name, services, SSIDs... (devinfo.h)
    if (d["upd"] | false) {   // signal update: moves the target, no feed row
        if (local()) ingest_sighting(ev.as<JsonObjectConst>());
        return;
    }
    uint32_t key = fnv(node.c_str(), mac.c_str()) ^ fnv(type);
    int slot = 0;
    for (int i = 0; i < DEDUP; i++) {
        if (dedup[i].h == key && now - dedup[i].ts < DEDUP_WINDOW) return;
        if (dedup[i].ts < dedup[slot].ts) slot = i;
    }
    dedup[slot] = {key, now};
    ev["alias"] = d["alias"] | "";
    ev["source"] = "usb";
    ev["is_new"] = known_add(mac.c_str());
    ev["id"] = nextId++;
    if (seenBy.size() > 1) ev["closest"] = seenBy[0][0];
    if (local()) ingest_det(ev.as<JsonObjectConst>(), true);
}

void on_line(char *line) {
    while (*line == ' ') line++;
    size_t n = strlen(line);
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\r')) line[--n] = 0;
    if (!n) return;
    lastLineMs = millis();
    if (!strncmp(line, "WD {", 4)) {
        // A wardriver's sighting: stamp it with our GPS fix and the node it came from,
        // keep it, and send the stamped line up to the dashboard.
        static char out[2400];
        GpsFix f = gps_fix();
        int k = snprintf(out, sizeof out, "WD {\"node\":\"%s\",", localId);
        bool has = strstr(line, "\"lat\":") != nullptr;   // a GPS wardriver stamps its own fix
        if (f.valid && !has) k += snprintf(out + k, sizeof out - k, "\"lat\":%.7f,\"lon\":%.7f,\"acc\":%.1f,", f.lat, f.lon, f.acc);
        strlcpy(out + k, line + 4, sizeof out - k);
        net_bridge_line(out);
        JsonDocument d(&psram);
        if (!deserializeJson(d, out + 3)) {
            model_lock();
            wd_ingest(d.as<JsonObjectConst>(), true);
            model_unlock();
        }
        return;
    }
    net_bridge_line(line);
    if (!strncmp(line, "GPS {", 5)) {   // a GPS node on our USB: its fix is ours too
        JsonDocument d(&psram);
        if (!deserializeJson(d, line + 4) && d["lat"].is<double>())
            gps_external(d["lat"], d["lon"], d["acc"] | 10.0f, d["alt"] | 0.0f, d["sats"] | 0, d["spd"] | 0.0f, d["crs"] | 0.0f);
        return;
    }
    if (!strncmp(line, "TLM ", 4)) on_tlm(line + 4);
    else if (!strncmp(line, "RST ", 4)) on_rst(line + 4);
    else if (!strncmp(line, "RID ", 4)) {   // a drone's Remote ID, from this node or relayed
        if (local()) {
            JsonDocument d(&psram);
            if (!deserializeJson(d, line + 4)) ingest_rid(d.as<JsonObjectConst>(), localId);
        }
    }
    else if (!strncmp(line, "DET ", 4) && strstr(line, "\"mac\"")) on_detection(line + 4);
    else if (line[0] == '{' && strstr(line, "\"mac\"")) on_detection(line);
}

void feed_task(void *) {
    static char buf[512];
    static char line[2048];
    size_t len = 0;
    uint32_t lastStatus = 0, lastRstatus = 0, lastTick = 0, lastStats = 0, gen = 0;
    int knownNodes = 0;
    bool wasLive = false;
    for (;;) {
        size_t got = usb_read(buf, sizeof buf, 100);
        for (size_t i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n') {
                line[len] = 0;
                on_line(line);
                len = 0;
            } else if (len < sizeof line - 1) {
                line[len++] = c;
            } else {
                len = 0;   // runaway line: drop it
            }
        }
        uint32_t ms = millis();
        bool live = usb_live();
        if (live != wasLive) {
            wasLive = live;
            model_lock();
            M->usbLive = live;
            M->version++;
            model_unlock();
            if (live) {
                lastStatus = lastRstatus = 0;
                toast("USB node connected");
            } else {
                toast("USB node disconnected");
            }
        }
        if (!live) continue;
        // Ask for settings (fw, filter CRC, alias): ours now and then, the fleet's when the
        // home has new company. Read-only console verbs.
        if (!local()) {   // the dashboard polls status itself through the bridge
            lastStatus = lastRstatus = ms;
            continue;
        }
        if (!lastStatus || ms - lastStatus > 60000) {
            lastStatus = ms;
            usb_send("status");
            usb_send("txlow 1");   // it runs on our port's power: full-power transmit peaks reset our Wi-Fi chip
        }
        // Every node's status (name, firmware, filters) over the mesh, through whichever node
        // is on USB: commands are signed with the fleet key, so a remote can ask as well as
        // the home. Soon after a new node shows up, then every 5 min.
        if (!lastRstatus || (nn > knownNodes ? ms - lastRstatus > 20000 : ms - lastRstatus > 300000)) {
            lastRstatus = ms;
            knownNodes = nn;
            usb_send("rstatus all");
        }
        if (ms - lastTick > 2000) {   // home timeout + ages
            lastTick = ms;
            publish_fleet();
            prune_targets();
        }
        if (gen != A.metricsGen || ms - lastStats > 5000) {
            gen = A.metricsGen;
            lastStats = ms;
            model_local_stats(RANGES[A.range % NRANGES], A.node, A.dom);
        }
    }
}

}  // namespace

bool usb_live() { return usb_attached() && lastLineMs && millis() - lastLineMs < 15000; }
const char *usb_node_id() { return localId; }

void feed_start() {
    seen = (SeenMac *)ps_calloc(SEEN_MACS, sizeof(SeenMac));
    known = (uint32_t *)ps_calloc(KNOWN, sizeof(uint32_t));
    usb_start();
    xTaskCreatePinnedToCore(feed_task, "feed", 12288, nullptr, 3, nullptr, 1);
}
