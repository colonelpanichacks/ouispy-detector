#include "model.h"
#include "settings.h"
#include <M5GFX.h>

Model *M = nullptr;
static SemaphoreHandle_t lock_;

void model_init() {
    M = (Model *)ps_calloc(1, sizeof(Model));
    lock_ = xSemaphoreCreateRecursiveMutex();
    strlcpy(M->mode, "live", sizeof M->mode);
    // Restore muted MACs.
    int start = 0;
    while (start < (int)S.muted.length() && M->nmuted < MAX_MUTED) {
        int comma = S.muted.indexOf(',', start);
        String one = S.muted.substring(start, comma < 0 ? S.muted.length() : comma);
        one.trim();
        if (one.length()) strlcpy(M->muted[M->nmuted++], one.c_str(), 18);
        if (comma < 0) break;
        start = comma + 1;
    }
}

void model_lock() { xSemaphoreTakeRecursive(lock_, portMAX_DELAY); }
void model_unlock() { xSemaphoreGiveRecursive(lock_); }

struct Guard {
    Guard() { model_lock(); }
    ~Guard() { model_unlock(); }
};

double server_now() {
    if (!M->serverMs) return 0;
    return M->serverTs + (millis() - M->serverMs) / 1000.0;
}

static void clock_from(double ts) {
    if (ts > 1e9) {
        M->serverTs = ts;
        M->serverMs = millis();
        M->clockValid = true;
    }
}

void model_set_clock(double ts, bool valid) {
    Guard g;
    M->serverTs = ts;
    M->serverMs = millis();
    M->clockValid = valid;
}

static void cpy(char *dst, size_t n, JsonVariantConst v) {
    const char *s = v.is<const char *>() ? v.as<const char *>() : "";
    strlcpy(dst, s, n);
}
#define CPY(dst, v) cpy(dst, sizeof(dst), v)

const Node *node_by_id(const char *id) {
    if (!id || !*id) return nullptr;
    for (int i = 0; i < M->nnodes; i++)
        if (!strcmp(M->nodes[i].id, id)) return &M->nodes[i];
    return nullptr;
}

const char *node_name(const char *id) {
    const Node *n = node_by_id(id);
    if (n && n->alias[0]) return n->alias;
    return (id && *id) ? id : "?";
}

static uint16_t hsl565(float h, float s, float l) {
    float c = (1 - fabsf(2 * l - 1)) * s;
    float hp = fmodf(h, 360) / 60.0f;
    float x = c * (1 - fabsf(fmodf(hp, 2) - 1));
    float r = 0, g = 0, b = 0;
    if (hp < 1) r = c, g = x;
    else if (hp < 2) r = x, g = c;
    else if (hp < 3) g = c, b = x;
    else if (hp < 4) g = x, b = c;
    else if (hp < 5) r = x, b = c;
    else r = c, b = x;
    float m = l - c / 2;
    return lgfx::color565((r + m) * 255, (g + m) * 255, (b + m) * 255);
}

uint16_t node_color565(const char *id) {
    const Node *n = node_by_id(id);
    if (n && n->hue >= 0) return hsl565(n->hue, 0.9f, 0.62f);
    if (n && n->isHome) return lgfx::color565(0x4e, 0xcd, 0xc4);
    uint32_t h = 2166136261u;   // stable per-id hue for nodes without a colour
    for (const char *p = id; p && *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return hsl565(h % 360, 0.9f, 0.62f);
}

const Det *det_at(int i) {
    if (i < 0 || i >= M->ndets) return nullptr;
    return &M->dets[(M->detHead - i + MAX_DETS) % MAX_DETS];
}

bool mac_muted(const char *mac) {
    for (int i = 0; i < M->nmuted; i++)
        if (!strcasecmp(M->muted[i], mac)) return true;
    return false;
}

static void save_muted() {
    String s;
    for (int i = 0; i < M->nmuted; i++) {
        if (i) s += ',';
        s += M->muted[i];
    }
    S.muted = s;
    settings_save();
}

void mute_toggle(const char *mac) {
    if (!mac || !*mac) return;
    {
        Guard g;
        for (int i = 0; i < M->nmuted; i++) {
            if (!strcasecmp(M->muted[i], mac)) {
                memmove(M->muted[i], M->muted[i + 1], (M->nmuted - i - 1) * 18);
                M->nmuted--;
                goto done;
            }
        }
        if (M->nmuted == MAX_MUTED) {   // full: forget the oldest
            memmove(M->muted[0], M->muted[1], (MAX_MUTED - 1) * 18);
            M->nmuted--;
        }
        strlcpy(M->muted[M->nmuted++], mac, 18);
        if (M->hasAlert && !strcasecmp(M->lastAlert.mac, mac)) M->alertsUnacked = 0;
    done:
        M->version++;
    }
    save_muted();
}

void alerts_ack() {
    Guard g;
    M->alertsUnacked = 0;
    M->version++;
}

static uint8_t parse_seen(JsonArrayConst arr, Seen *out) {
    uint8_t n = 0;
    for (JsonArrayConst row : arr) {
        if (n == MAX_SEEN) break;
        CPY(out[n].node, row[0]);
        out[n].rssi = row[1] | 0;
        out[n].age = row[2] | 0;
        n++;
    }
    return n;
}

// ---------------------------------------------------------------- fleet

static void parse_node(Node &n, JsonObjectConst o) {
    memset(&n, 0, sizeof n);
    CPY(n.id, o["id"]);
    CPY(n.alias, o["alias"]);
    CPY(n.role, o["role"]);
    n.online = o["online"] | false;
    n.isHome = o["is_home"] | false;
    n.rssi = o["rssi"] | 0;
    n.age = o["age"] | -1.0f;
    n.since = o["since"] | 0.0;
    n.hue = o["color"].is<int>() ? (int)o["color"] : -1;
    n.hasPos = o["lat"].is<double>() && o["lon"].is<double>();
    n.lat = o["lat"] | 0.0;
    n.lon = o["lon"] | 0.0;
    JsonVariantConst fr = o["filters_fresh"];
    n.filtersFresh = fr.isNull() ? -1 : (fr.as<bool>() ? 1 : 0);
    JsonObjectConst cfg = o["cfg"];
    n.filters = -1;
    if (!cfg.isNull()) {
        CPY(n.fw, cfg["fw"]);
        CPY(n.fcrc, cfg["fcrc"]);
        CPY(n.mode, cfg["mode"]);
        n.filters = cfg["filters"] | -1;
        n.lrssi = cfg["lrssi"] | 0;
        n.peers = cfg["peers"] | 0;
        n.ch = cfg["ch"] | 0;
        n.dev = cfg["dev"] | 0;
        n.up = cfg["up"] | 0;
        n.heap = cfg["heap"] | 0;
        n.nnbr = parse_seen(cfg["nbr"], n.nbr);
    }
    JsonObjectConst ota = o["ota"];
    if (!ota.isNull()) {
        CPY(n.ota, ota["state"]);
        n.otaPct = ota["pct"] | 0;
    }
}

static void fleet_locked(JsonObjectConst f) {
    JsonObjectConst h = f["home"];
    Home &home = M->home;
    CPY(home.id, h["id"]);
    CPY(home.source, h["source"]);
    CPY(home.rate, h["rate"]);
    home.alive = h["alive"] | false;
    home.legacy = h["legacy"] | false;
    home.enc = h["enc"] | 0;
    home.mesh = h["mesh"] | 0;
    home.ch = h["ch"] | 0;
    home.rx = h["rx"] | 0;
    home.dup = h["dup"] | 0;
    home.tx = h["tx"] | 0;
    home.txf = h["txf"] | 0;
    home.pend = h["pend"] | 0;
    home.bad = h["bad"] | 0;
    home.up = h["up"] | 0;
    home.heap = h["heap"] | 0;
    home.lrssi = h["lrssi"] | 0;
    home.age = h["age"] | -1.0f;
    int n = 0;
    for (JsonObjectConst o : f["nodes"].as<JsonArrayConst>()) {
        if (n == MAX_NODES) break;
        parse_node(M->nodes[n++], o);
    }
    M->nnodes = n;
    clock_from(f["ts"] | 0.0);
    M->version++;
}

void ingest_fleet(JsonObjectConst f) {
    Guard g;
    fleet_locked(f);
}

// ---------------------------------------------------------------- local stats (USB mode)

static uint32_t hash_mac(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1;
}

// Tiny open-addressing set of 32-bit keys; true if the key was new.
static bool set_add(uint32_t *set, uint32_t cap, uint32_t key) {
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t &slot = set[(key + i) & (cap - 1)];
        if (slot == key) return false;
        if (!slot) {
            slot = key;
            return true;
        }
    }
    return false;
}

static void bar_add(Bar *bars, uint8_t &n, const char *name, uint32_t k, uint32_t *devSets, bool isNewDev) {
    int i = 0;
    while (i < n && strcmp(bars[i].name, name)) i++;
    if (i == n) {
        if (n == MAX_BARS) return;
        memset(&bars[n], 0, sizeof(Bar));
        strlcpy(bars[n].name, name, sizeof bars[n].name);
        n++;
    }
    bars[i].count++;
    (void)k, (void)devSets;
    if (isNewDev) bars[i].devices++;
}

void model_local_stats(uint32_t range, const char *node, uint8_t dom) {
    constexpr uint32_t CAP = 8192;
    uint32_t *macs = (uint32_t *)ps_calloc(CAP, 4), *pairs = (uint32_t *)ps_calloc(CAP, 4),
             *all = (uint32_t *)ps_calloc(CAP, 4), *vdev = (uint32_t *)ps_calloc(CAP, 4),
             *ndev = (uint32_t *)ps_calloc(CAP, 4);
    if (!macs || !pairs || !all || !vdev || !ndev) {
        free(macs), free(pairs), free(all), free(vdev), free(ndev);
        return;
    }
    {
        Guard g;
        double now = server_now();
        Metrics &m = M->metrics;
        memset(&m, 0, sizeof m);
        m.valid = true;
        m.range = range;
        m.bucket = range <= 3600 ? 60 : range <= 6 * 3600 ? 300 : range <= 86400 ? 900 : 3600;
        m.until = now;
        m.start = floor((now - range) / m.bucket) * m.bucket;
        m.nbuckets = min((int)MAX_BUCKETS, (int)ceil((now - m.start) / m.bucket));
        m.rssiFrom = -100;
        m.rssiStep = 5;
        m.nrssi = 17;
        uint32_t nodesSeen[MAX_NODES] = {};
        uint8_t nNodes = 0;
        Stats &st = M->stats;
        memset(&st, 0, sizeof st);
        st.nhourly = 24;
        double hour0 = floor(now / 3600) * 3600 - 23 * 3600;
        for (int i = 0; i < M->ndets; i++) {
            const Det *d = det_at(i);
            uint32_t mh = hash_mac(d->mac);
            // Stats: everything, regardless of filters (like /api/stats).
            st.total++;
            if (set_add(all, CAP, mh)) st.macs++;
            if (now - d->ts < 86400) st.day++;
            if (d->ts >= hour0) {
                int h = (int)((d->ts - hour0) / 3600);
                if (h >= 0 && h < 24) st.hourly[h]++;
            }
            // Metrics: the range and the filters.
            if (node && *node && strcmp(d->node, node)) continue;
            if (dom == 1 && strcmp(d->dom, "ble")) continue;
            if (dom == 2 && strcmp(d->dom, "wifi")) continue;
            if (d->ts < now - 2.0 * range) continue;
            if (d->ts < now - range) {
                m.prevTotal++;
                continue;
            }
            m.total++;
            m.n++;
            if (set_add(macs, CAP, mh)) m.unique++;
            if (d->isNew) m.newDevices++;
            int b = (int)((d->ts - m.start) / m.bucket);
            if (b >= 0 && b < m.nbuckets) {
                m.series[b]++;
                if (set_add(pairs, CAP, mh * 31 + b + 1)) m.uniqueSeries[b]++;
            }
            const char *vendor = d->descr[0] ? d->descr : d->type;
            bar_add(m.vendor, m.nvendor, vendor, mh, vdev, set_add(vdev, CAP, mh ^ hash_mac(vendor)));
            bar_add(m.node, m.nnode, d->node, mh, ndev, set_add(ndev, CAP, mh ^ hash_mac(d->node)));
            bar_add(m.type, m.ntype, d->type, mh, nullptr, false);
            if (!strcmp(d->dom, "ble")) m.ble++;
            else m.wifi++;
            int rb = (d->rssi - m.rssiFrom) / m.rssiStep;
            if (d->rssi && rb >= 0 && rb < m.nrssi) m.rssi[rb]++;
            uint32_t nh = hash_mac(d->node);
            bool have = false;
            for (int k = 0; k < nNodes; k++) have |= nodesSeen[k] == nh;
            if (!have && nNodes < MAX_NODES) nodesSeen[nNodes++] = nh;
        }
        m.nodesActive = nNodes;
        for (int b = 0; b < m.nbuckets; b++)
            if (m.series[b] > m.peak) m.peak = m.series[b], m.peakTs = m.start + b * m.bucket;
        auto bySize = [](const Bar &a, const Bar &b) { return a.count > b.count; };
        std::sort(m.vendor, m.vendor + m.nvendor, bySize);
        std::sort(m.node, m.node + m.nnode, bySize);
        std::sort(m.type, m.type + m.ntype, bySize);
        m.fetchedMs = millis();
        M->version++;
    }
    free(macs), free(pairs), free(all), free(vdev), free(ndev);
}

// ---------------------------------------------------------------- detections

static Target *target_for(const char *mac, bool create) {
    for (int i = 0; i < M->ntargets; i++)
        if (!strcmp(M->targets[i].mac, mac)) return &M->targets[i];
    if (!create) return nullptr;
    Target *t;
    if (M->ntargets < MAX_TARGETS) {
        t = &M->targets[M->ntargets++];
    } else {   // evict the one heard longest ago
        t = &M->targets[0];
        for (int i = 1; i < M->ntargets; i++)
            if (M->targets[i].last < t->last) t = &M->targets[i];
    }
    memset(t, 0, sizeof *t);
    strlcpy(t->mac, mac, sizeof t->mac);
    return t;
}

static void target_update(const char *mac, const char *descr, const char *type, const char *dom,
                          double ts, JsonArrayConst seen, bool hit) {
    Target *t = target_for(mac, true);
    if (!t->first) t->first = ts;
    if (ts >= t->last) {
        t->last = ts;
        if (descr && *descr) strlcpy(t->descr, descr, sizeof t->descr);
        if (type && *type) strlcpy(t->type, type, sizeof t->type);
        if (dom && *dom) strlcpy(t->dom, dom, sizeof t->dom);
        if (!seen.isNull()) t->nseen = parse_seen(seen, t->seen);
    }
    if (hit) t->hits++;
}

static void det_from(Det &d, JsonObjectConst o) {
    memset(&d, 0, sizeof d);
    d.id = o["id"] | 0;
    d.ts = o["ts"] | 0.0;
    CPY(d.node, o["node"]);
    CPY(d.mac, o["mac"]);
    CPY(d.descr, o["descr"]);
    CPY(d.alias, o["alias"]);
    CPY(d.type, o["type"]);
    CPY(d.dom, o["dom"]);
    CPY(d.closest, o["closest"]);
    CPY(d.name, o["name"]);
    if (!d.name[0] && o["info"].is<JsonObjectConst>()) {   // USB mode: straight from the node
        JsonObjectConst in = o["info"];
        if (in["name"].is<const char *>()) CPY(d.name, in["name"]);
        else if (in["ssid"].is<const char *>() && (!strcmp(in["frame"] | "", "beacon") || !strcmp(in["frame"] | "", "probe-resp")))
            CPY(d.name, in["ssid"]);
    }
    if (o["group"].is<JsonObjectConst>()) CPY(d.group, o["group"]["name"]);
    d.rssi = o["rssi"] | 0;
    d.isNew = o["is_new"] | false;
    d.nseen = parse_seen(o["seen_by"], d.seen);
}

// Names, alias, group and the latest intel blob follow the device into its target.
static void target_names(const Det &d, JsonObjectConst o) {
    Target *t = target_for(d.mac, false);
    if (!t) return;
    if (d.name[0]) strlcpy(t->name, d.name, sizeof t->name);
    strlcpy(t->alias, d.alias, sizeof t->alias);
    strlcpy(t->group, d.group, sizeof t->group);
    if (o["info"].is<JsonObjectConst>()) serializeJson(o["info"], t->info, sizeof t->info);
}

static void ring_push(const Det &d) {
    M->detHead = (M->detHead + 1) % MAX_DETS;
    M->dets[M->detHead] = d;
    if (M->ndets < MAX_DETS) M->ndets++;
    if (d.id > M->maxDetId) M->maxDetId = d.id;
}

void ingest_det(JsonObjectConst o, bool live) {
    Guard g;
    Det d;
    det_from(d, o);
    if (!d.mac[0]) return;
    if (d.id && d.id <= M->maxDetId) {   // already have it (state load raced the event)
        for (int i = 0; i < M->ndets; i++)
            if (det_at(i)->id == d.id) return;
    }
    Target *prev = target_for(d.mac, false);
    bool cooled = !prev || d.ts - prev->last > ALERT_COOLDOWN_S;
    ring_push(d);
    target_update(d.mac, d.descr, d.type, d.dom, d.ts, o["seen_by"], true);
    target_names(d, o);
    clock_from(d.ts);
    if (live && cooled && !mac_muted(d.mac)) {
        M->lastAlert = d;
        M->hasAlert = true;
        M->alertsUnacked++;
        M->alertSeq++;
    }
    M->events++;
    M->version++;
}

void ingest_sighting(JsonObjectConst s) {
    Guard g;
    const char *mac = s["mac"] | "";
    if (!*mac) return;
    double ts = s["ts"] | 0.0;
    target_update(mac, s["descr"] | "", s["type"] | "", s["dom"] | "", ts, s["seen_by"], false);
    if (Target *t = target_for(mac, false)) {
        if (s["name"].is<const char *>() && *(const char *)s["name"]) CPY(t->name, s["name"]);
        if (s["info"].is<JsonObjectConst>()) serializeJson(s["info"], t->info, sizeof t->info);
    }
    clock_from(ts);
    M->version++;
}

void ingest_cleared(JsonObjectConst e) {
    Guard g;
    const char *node = e["node"] | "";
    const char *mac = e["mac"] | "";
    // Compact the ring, keeping order, dropping what the dashboard cleared.
    Det *keep = (Det *)ps_malloc(sizeof(Det) * MAX_DETS);
    if (!keep) return;
    int n = 0;
    for (int i = M->ndets - 1; i >= 0; i--) {
        const Det *d = det_at(i);
        bool hit = (!*node || !strcmp(d->node, node)) && (!*mac || !strcasecmp(d->mac, mac));
        if (!hit) keep[n++] = *d;
    }
    M->ndets = 0;
    M->detHead = MAX_DETS - 1;
    for (int i = 0; i < n; i++) ring_push(keep[i]);
    free(keep);
    if (!*node && !*mac) M->ntargets = 0;
    M->version++;
}

void prune_targets() {
    Guard g;
    double now = server_now();
    if (!now) return;
    int w = 0;
    for (int i = 0; i < M->ntargets; i++)
        if (now - M->targets[i].last < TARGET_KEEP_S) M->targets[w++] = M->targets[i];
    if (w != M->ntargets) {
        M->ntargets = w;
        M->version++;
    }
}

// ---------------------------------------------------------------- polls

static void stats_locked(JsonVariantConst st, JsonArrayConst hourly) {
    M->stats.day = st["day"] | 0;
    M->stats.total = st["total"] | 0;
    M->stats.macs = st["macs"] | 0;
    uint8_t n = 0;
    for (JsonObjectConst h : hourly) {
        if (n == 24) break;
        M->stats.hourly[n++] = h["c"] | 0;
    }
    M->stats.nhourly = n;
}

void ingest_stats(JsonDocument &doc) {
    Guard g;
    stats_locked(doc.as<JsonVariantConst>(), doc["hourly"]);
    M->version++;
}

void ingest_state(JsonDocument &doc) {
    Guard g;
    CPY(M->mode, doc["mode"]);
    if (!M->mode[0]) strlcpy(M->mode, "live", sizeof M->mode);
    fleet_locked(doc["fleet"]);
    stats_locked(doc["stats"], doc["hourly"]);

    // Rebuild the feed from the server's newest 150, then put back anything newer that
    // arrived over the socket while this was in flight.
    Det *newer = (Det *)ps_malloc(sizeof(Det) * MAX_DETS);
    int nn = 0;
    JsonArrayConst recent = doc["recent"];
    uint32_t stateMax = 0;
    for (JsonObjectConst o : recent) stateMax = max(stateMax, (uint32_t)(o["id"] | 0));
    if (newer) {
        for (int i = M->ndets - 1; i >= 0; i--)
            if (det_at(i)->id > stateMax) newer[nn++] = *det_at(i);
    }
    M->ndets = 0;
    M->detHead = MAX_DETS - 1;
    M->maxDetId = 0;
    M->ntargets = 0;
    for (int i = (int)recent.size() - 1; i >= 0; i--) {   // oldest first
        JsonObjectConst o = recent[i];
        Det d;
        det_from(d, o);
        if (!d.mac[0]) continue;
        ring_push(d);
        target_update(d.mac, d.descr, d.type, d.dom, d.ts, o["seen_by"], true);
        target_names(d, o);
    }
    for (int i = 0; i < nn; i++) {
        ring_push(newer[i]);
    }
    free(newer);
    for (JsonObjectConst dr : doc["drones"].as<JsonArrayConst>()) ingest_drone(dr);
    for (JsonObjectConst fx : doc["tri"].as<JsonArrayConst>()) ingest_tri(fx);
    ingest_names(doc["mac_aliases"], doc["device_names"], doc["group_of"]);
    M->stateLoaded = true;
    M->version++;
}

void ingest_metrics(JsonDocument &doc, uint32_t range) {
    Guard g;
    Metrics &m = M->metrics;
    m.valid = true;
    m.range = range;
    m.bucket = doc["bucket"] | 0;
    m.start = doc["start"] | 0.0;
    m.until = doc["until"] | 0.0;
    m.total = doc["total"] | 0;
    m.prevTotal = doc["prev_total"] | 0;
    m.unique = doc["unique"] | 0;
    m.newDevices = doc["new_devices"] | 0;
    m.nodesActive = doc["nodes_active"] | 0;
    m.n = doc["n"] | 0;
    m.peak = doc["peak"]["count"] | 0;
    m.peakTs = doc["peak"]["ts"] | 0.0;

    JsonArrayConst series = doc["series"];
    size_t len = 0;
    for (JsonObjectConst s : series) len = max(len, s["values"].as<JsonArrayConst>().size());
    JsonArrayConst us = doc["unique_series"];
    len = max(len, us.size());
    size_t skip = len > MAX_BUCKETS ? len - MAX_BUCKETS : 0;
    m.nbuckets = len - skip;
    memset(m.series, 0, sizeof m.series);
    memset(m.uniqueSeries, 0, sizeof m.uniqueSeries);
    for (JsonObjectConst s : series) {
        size_t i = 0;
        for (JsonVariantConst v : s["values"].as<JsonArrayConst>()) {
            if (i >= skip && i - skip < MAX_BUCKETS) m.series[i - skip] += (int)(v | 0);
            i++;
        }
    }
    {
        size_t i = 0;
        for (JsonVariantConst v : us) {
            if (i >= skip && i - skip < MAX_BUCKETS) m.uniqueSeries[i - skip] = v | 0;
            i++;
        }
    }
    auto bars = [](JsonArrayConst arr, const char *key, Bar *out, uint8_t &n) {
        n = 0;
        for (JsonObjectConst o : arr) {
            if (n == MAX_BARS) break;
            cpy(out[n].name, sizeof out[n].name, o[key]);
            out[n].count = o["count"] | 0;
            out[n].devices = o["devices"] | 0;
            n++;
        }
    };
    bars(doc["by_vendor"], "name", m.vendor, m.nvendor);
    bars(doc["by_node"], "id", m.node, m.nnode);
    bars(doc["by_type"], "type", m.type, m.ntype);
    m.ble = doc["by_dom"]["ble"] | 0;
    m.wifi = doc["by_dom"]["wifi"] | 0;
    m.rssiFrom = doc["rssi"]["from"] | -100;
    m.rssiStep = doc["rssi"]["step"] | 5;
    m.nrssi = 0;
    for (JsonVariantConst v : doc["rssi"]["counts"].as<JsonArrayConst>()) {
        if (m.nrssi == 24) break;
        m.rssi[m.nrssi++] = v | 0;
    }
    m.fetchedMs = millis();
    clock_from(m.until);
    M->version++;
}

// ---------------------------------------------------------------- drones

static Drone *drone_for(const char *mac) {
    for (int i = 0; i < M->ndrones; i++)
        if (!strcasecmp(M->drones[i].mac, mac)) return &M->drones[i];
    Drone *d;
    if (M->ndrones < MAX_DRONES) d = &M->drones[M->ndrones++];
    else {
        d = &M->drones[0];
        for (int i = 1; i < M->ndrones; i++)
            if (M->drones[i].last < d->last) d = &M->drones[i];
    }
    memset(d, 0, sizeof *d);
    strlcpy(d->mac, mac, sizeof d->mac);
    d->pilotDist = -1;
    strlcpy(d->tag, "unknown", sizeof d->tag);
    M->droneSeq++;
    return d;
}

static double hav_m(double a1, double o1, double a2, double o2) {
    double r = M_PI / 180, dl = (a2 - a1) * r, dn = (o2 - o1) * r;
    double h = sin(dl / 2) * sin(dl / 2) + cos(a1 * r) * cos(a2 * r) * sin(dn / 2) * sin(dn / 2);
    return 6371000.0 * 2 * asin(sqrt(h));
}

static void drone_point(Drone *d) {
    if (!d->hasPos) return;
    if (d->ntrack && fabsf(d->track[d->ntrack - 1][0] - (float)d->lat) < 1e-6f &&
        fabsf(d->track[d->ntrack - 1][1] - (float)d->lon) < 1e-6f) return;
    if (d->ntrack == DRONE_TRACK) {
        memmove(d->track[0], d->track[1], sizeof(d->track[0]) * (DRONE_TRACK - 1));
        d->ntrack--;
    }
    d->track[d->ntrack][0] = d->lat;
    d->track[d->ntrack][1] = d->lon;
    d->ntrack++;
}

// The dashboard's "drone" event (drones.py _public) and /api/state "drones".
void ingest_drone(JsonObjectConst o) {
    Guard g;
    const char *mac = o["mac"] | "";
    if (!*mac) return;
    Drone *d = drone_for(mac);
    CPY(d->alias, o["alias"]);
    if (o["id"].is<const char *>()) CPY(d->id, o["id"]);
    if (o["op"].is<const char *>()) CPY(d->op, o["op"]);
    if (o["desc"].is<const char *>()) CPY(d->desc, o["desc"]);
    if (o["ua_type"].is<const char *>()) CPY(d->ua, o["ua_type"]);
    if (o["status"].is<const char *>()) CPY(d->status, o["status"]);
    if (o["tag"].is<const char *>()) CPY(d->tag, o["tag"]);
    if (o["src"].is<const char *>()) CPY(d->src, o["src"]);
    if (o["closest"].is<const char *>()) CPY(d->closest, o["closest"]);
    if (o["lat"].is<double>()) d->lat = o["lat"], d->lon = o["lon"] | 0.0, d->hasPos = true;
    if (o["plat"].is<double>()) d->plat = o["plat"], d->plon = o["plon"] | 0.0, d->hasPilot = true;
    if (o["alt"].is<float>()) d->alt = o["alt"], d->hasAlt = true;
    if (o["hagl"].is<float>()) d->hagl = o["hagl"], d->hasHagl = true;
    if (o["spd"].is<float>()) d->spd = o["spd"], d->hasSpd = true;
    if (o["vspd"].is<float>()) d->vspd = o["vspd"];
    if (o["hdg"].is<float>()) d->hdg = o["hdg"], d->hasHdg = true;
    d->rssi = o["rssi"] | d->rssi;
    d->pilotDist = o["pilot_dist_m"] | -1;
    d->first = o["first"] | d->first;
    d->last = o["last"] | d->last;
    d->reports = o["reports"] | d->reports;
    JsonArrayConst tr = o["track"];
    if (!tr.isNull() && tr.size()) {
        d->ntrack = 0;
        int skip = max(0, (int)tr.size() - DRONE_TRACK);
        int k = 0;
        for (JsonArrayConst p : tr) {
            if (k++ < skip) continue;
            d->track[d->ntrack][0] = p[1] | 0.0f;
            d->track[d->ntrack][1] = p[2] | 0.0f;
            d->ntrack++;
        }
    } else {
        drone_point(d);
    }
    clock_from(d->last);
    M->version++;
}

static const char *UA_NAMES[] = {"None", "Aeroplane", "Multirotor", "Gyroplane", "Hybrid VTOL", "Ornithopter",
                                 "Glider", "Kite", "Free balloon", "Captive balloon", "Airship", "Parachute",
                                 "Rocket", "Tethered", "Ground obstacle", "Other"};
static const char *ST_NAMES[] = {"Undeclared", "Ground", "Airborne", "EMERGENCY", "RID failure"};

// A node's own "RID {...}" line (USB mode: no dashboard to merge for us).
void ingest_rid(JsonObjectConst o, const char *homeId) {
    Guard g;
    const char *mac = o["mac"] | "";
    if (!*mac) return;
    char up[18];
    strlcpy(up, mac, sizeof up);
    for (char *p = up; *p; p++) *p = toupper(*p);
    Drone *d = drone_for(up);
    double now = server_now();
    if (!d->first) d->first = now;
    d->last = now;
    d->reports++;
    d->rssi = o["rssi"] | 0;
    CPY(d->closest, o["node"]);
    if (!d->closest[0] && homeId) strlcpy(d->closest, homeId, sizeof d->closest);
    CPY(d->src, o["src"]);
    if (o["id"].is<const char *>()) CPY(d->id, o["id"]);
    if (o["op"].is<const char *>()) CPY(d->op, o["op"]);
    if (o["desc"].is<const char *>()) CPY(d->desc, o["desc"]);
    if (o["uat"].is<int>()) strlcpy(d->ua, UA_NAMES[constrain((int)o["uat"], 0, 15)], sizeof d->ua);
    if (o["st"].is<int>()) strlcpy(d->status, ST_NAMES[constrain((int)o["st"], 0, 4)], sizeof d->status);
    double lat = o["lat"] | 0.0, lon = o["lon"] | 0.0;
    if (lat || lon) d->lat = lat, d->lon = lon, d->hasPos = true;
    double pl = o["plat"] | 0.0, po = o["plon"] | 0.0;
    if (pl || po) d->plat = pl, d->plon = po, d->hasPilot = true;
    float alt = o["alt"] | -1000.0f, hagl = o["hagl"] | -1000.0f, spd = o["spd"] | 255.0f, hdg = o["hdg"] | 361.0f;
    if (alt > -1000) d->alt = alt, d->hasAlt = true;
    if (hagl > -1000) d->hagl = hagl, d->hasHagl = true;
    if (spd < 254) d->spd = spd, d->hasSpd = true;
    if (hdg <= 360) d->hdg = hdg, d->hasHdg = true;
    d->vspd = o["vspd"] | 0.0f;
    d->pilotDist = d->hasPos && d->hasPilot ? (int32_t)hav_m(d->lat, d->lon, d->plat, d->plon) : -1;
    drone_point(d);
    M->version++;
}

int live_drones() {
    int n = 0;
    double now = server_now();
    for (int i = 0; i < M->ndrones; i++) n += now - M->drones[i].last < DRONE_LIVE_S;
    return n;
}

// /api/devices/<mac>?flat=1
void ingest_detail(JsonDocument &doc) {
    Guard g;
    DevDetail &dd = M->detail;
    if (strcasecmp(dd.mac, doc["mac"] | "")) return;   // the person moved on
    CPY(dd.name, doc["name"]);
    CPY(dd.alias, doc["alias"]);
    dd.group[0] = 0;
    if (doc["group"].is<JsonObjectConst>()) CPY(dd.group, doc["group"]["name"]);
    dd.nrows = 0;
    for (JsonArrayConst r : doc["rows"].as<JsonArrayConst>()) {
        if (dd.nrows == 30) break;
        CPY(dd.rows[dd.nrows][0], r[0]);
        CPY(dd.rows[dd.nrows][1], r[1]);
        dd.nrows++;
    }
    dd.ncomp = 0;
    for (JsonObjectConst c : doc["companions"].as<JsonArrayConst>()) {
        if (dd.ncomp == 8) break;
        auto &e = dd.comp[dd.ncomp++];
        CPY(e.mac, c["mac"]);
        CPY(e.label, c["label"]);
        CPY(e.group, c["group"]);
        e.together = c["together"] | 0;
        e.occasions = c["occasions"] | 0;
    }
    dd.ngroups = 0;
    for (JsonObjectConst gr : doc["groups"].as<JsonArrayConst>()) {
        if (dd.ngroups == 16) break;
        auto &e = dd.groups[dd.ngroups++];
        e.id = gr["id"] | 0;
        CPY(e.name, gr["name"]);
        e.n = gr["n"] | 0;
    }
    dd.loading = false;
    dd.valid = true;
    M->version++;
}

// Dashboard renamed / regrouped devices: refresh what is on screen.
void ingest_names(JsonObjectConst aliases, JsonObjectConst names, JsonObjectConst groupOf) {
    Guard g;
    for (int i = 0; i < M->ntargets; i++) {
        Target &t = M->targets[i];
        if (!aliases.isNull()) {
            JsonVariantConst a = aliases[t.mac]["alias"];
            strlcpy(t.alias, a.is<const char *>() ? a.as<const char *>() : "", sizeof t.alias);
        }
        if (!names.isNull() && names[t.mac].is<const char *>()) CPY(t.name, names[t.mac]);
        if (!groupOf.isNull()) {
            JsonVariantConst gn = groupOf[t.mac]["name"];
            strlcpy(t.group, gn.is<const char *>() ? gn.as<const char *>() : "", sizeof t.group);
        }
    }
    for (int i = 0; i < M->ndets; i++) {
        Det &d = M->dets[(M->detHead - i + MAX_DETS) % MAX_DETS];
        if (!aliases.isNull()) {
            JsonVariantConst a = aliases[d.mac]["alias"];
            if (a.is<const char *>()) CPY(d.alias, a);
        }
        if (!names.isNull() && names[d.mac].is<const char *>()) CPY(d.name, names[d.mac]);
        if (!groupOf.isNull()) {
            JsonVariantConst gn = groupOf[d.mac]["name"];
            strlcpy(d.group, gn.is<const char *>() ? gn.as<const char *>() : "", sizeof d.group);
        }
    }
    M->version++;
}

// A triangulated fix from the dashboard: where the device is, and how sure.
void ingest_tri(JsonObjectConst f) {
    Guard g;
    const char *mac = f["mac"] | "";
    if (!*mac) return;
    char up[18];
    strlcpy(up, mac, sizeof up);
    for (char *p = up; *p; p++) *p = toupper(*p);
    Target *t = target_for(up, false);
    if (!t) return;
    t->triOk = f["ok"] | false;
    t->triRough = f["rough"] | false;
    t->triLat = f["lat"] | 0.0;
    t->triLon = f["lon"] | 0.0;
    t->triErr = f["err"] | 0.0f;
    t->triTs = f["ts"] | 0.0;
    M->version++;
}
