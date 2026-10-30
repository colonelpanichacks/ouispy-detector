#include "wd.h"
#include <esp_heap_caps.h>

namespace {

WdDev *devs = nullptr;
int n = 0;
uint32_t ver = 0;
uint32_t sessWifi = 0, sessBle = 0;
// Open-addressed index into devs (by MAC + kind) so a busy drive stays cheap.
constexpr int IDX = 16384;
int16_t *idx = nullptr;

uint32_t hkey(const uint8_t *m, uint8_t kind) {
    uint32_t h = 2166136261u ^ kind;
    for (int i = 0; i < 6; i++) h = (h ^ m[i]) * 16777619u;
    return h;
}

bool parse_mac(const char *s, uint8_t *m) {
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) m[i] = v[i];
    return true;
}

uint8_t sec_code(const char *s) {
    if (!s || !*s) return 255;
    if (!strcmp(s, "open")) return 0;
    if (!strcmp(s, "WEP")) return 1;
    if (!strcmp(s, "WPA")) return 2;
    if (!strcmp(s, "WPA2")) return 3;
    if (!strcmp(s, "WPA3")) return 4;
    if (!strcmp(s, "WPA2/WPA3")) return 5;
    return 255;
}

bool ready() {
    if (devs) return true;
    devs = (WdDev *)heap_caps_calloc(WD_MAX, sizeof(WdDev), MALLOC_CAP_SPIRAM);
    idx = (int16_t *)heap_caps_malloc(IDX * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!devs || !idx) return false;
    memset(idx, 0xFF, IDX * sizeof(int16_t));
    return true;
}

WdDev *find(const uint8_t *m, uint8_t kind, bool create, bool &isNew) {
    isNew = false;
    uint32_t h = hkey(m, kind) % IDX;
    for (int k = 0; k < IDX; k++, h = (h + 1) % IDX) {
        int i = idx[h];
        if (i < 0) {
            if (!create) return nullptr;
            if (n >= WD_MAX) return nullptr;   // full: the dashboard keeps everything; the map shows these
            i = n++;
            idx[h] = i;
            memset(&devs[i], 0, sizeof(WdDev));
            memcpy(devs[i].mac, m, 6);
            devs[i].kind = kind;
            devs[i].sec = 255;
            devs[i].rssi = -127;
            isNew = true;
            return &devs[i];
        }
        if (i >= 0 && devs[i].kind == kind && !memcmp(devs[i].mac, m, 6)) return &devs[i];
    }
    return nullptr;
}

}  // namespace

void wd_ingest(JsonObjectConst o, bool session) {
    if (!ready()) return;
    const char *t = o["t"] | o["kind"] | "";
    uint8_t kind = !strcmp(t, "wifi") ? 1 : !strcmp(t, "ble") ? 2 : 0;
    uint8_t m[6];
    if (!kind || !parse_mac(o["mac"] | "", m)) return;
    bool isNew;
    WdDev *d = find(m, kind, true, isNew);
    if (!d) return;
    const char *nm = kind == 1 ? (o["ssid"] | o["name"] | "") : (o["name"] | "");
    if (*nm) strlcpy(d->name, nm, sizeof d->name);
    if (o["sec"].is<const char *>()) { uint8_t s = sec_code(o["sec"]); if (s != 255) d->sec = s; }
    if (o["ch"].is<int>()) d->ch = o["ch"];
    int rssi = o["rssi"] | -127;
    double la = o["lat"] | 0.0, lo = o["lon"] | 0.0;
    bool pos = (la || lo) && fabs(la) <= 90 && fabs(lo) <= 180;
    // Placed where it was loudest (with a position).
    if (pos && (!d->placed || rssi > d->rssi)) d->lat = la, d->lon = lo, d->placed = true;
    if (rssi > d->rssi) d->rssi = rssi;
    double last = o["last"] | 0.0;
    if (last > d->last) d->last = last;
    else if (!last) d->last = time(nullptr);
    double first = o["first"] | 0.0;
    if (first && (!d->first || first < d->first)) d->first = first;
    else if (!d->first) d->first = d->last;
    const char *nd = o["node"] | "";
    if (*nd) strlcpy(d->node, nd, sizeof d->node);
    d->seen = max<uint32_t>(d->seen + 1, o["seen"] | 0u);
    bool sess = session || (o["now"] | false);
    if (sess && !d->session) {
        d->session = true;
        (kind == 1 ? sessWifi : sessBle)++;
    }
    ver++;
}

int wd_count() { return n; }
const WdDev &wd_at(int i) { return devs[i]; }
uint32_t wd_session_count(uint8_t kind) { return kind == 1 ? sessWifi : kind == 2 ? sessBle : sessWifi + sessBle; }
uint32_t wd_version() { return ver; }

void wd_new_session() {
    for (int i = 0; i < n; i++) devs[i].session = false;
    sessWifi = sessBle = 0;
    ver++;
}

// The whole store as one block for store.cpp: {u32 count, u32 sessWifi, u32 sessBle}
// then the devices. wd_restore rebuilds the index. Both under the model lock.
size_t wd_snapshot(uint8_t *out, size_t cap) {
    size_t need = 12 + (size_t)n * sizeof(WdDev);
    if (!devs || need > cap) return 0;
    uint32_t h[3] = {(uint32_t)n, sessWifi, sessBle};
    memcpy(out, h, 12);
    memcpy(out + 12, devs, (size_t)n * sizeof(WdDev));
    return need;
}

bool wd_restore(const uint8_t *in, size_t len) {
    if (len < 12 || !ready()) return false;
    uint32_t h[3];
    memcpy(h, in, 12);
    if (h[0] > WD_MAX || len != 12 + (size_t)h[0] * sizeof(WdDev)) return false;
    memcpy(devs, in + 12, len - 12);
    n = h[0];
    sessWifi = h[1], sessBle = h[2];
    memset(idx, 0xFF, IDX * sizeof(int16_t));
    for (int i = 0; i < n; i++) {
        uint32_t k = hkey(devs[i].mac, devs[i].kind) % IDX;
        while (idx[k] >= 0) k = (k + 1) % IDX;
        idx[k] = i;
    }
    ver++;
    return true;
}
