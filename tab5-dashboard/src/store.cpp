#include "store.h"
#include "model.h"
#include "sdcard.h"
#include "wd.h"
#include <sys/stat.h>

// File: "OSST1\n\0\0", then blocks of {u32 tag, u32 length, data}. A block whose length
// isn't what this build expects (a struct changed in an update) is skipped, not misread,
// so an update loses only the parts it changed. Written to snap.tmp, then swapped in with
// the old copy kept as snap.old until the new one is in place: pulling the power mid-write
// never leaves nothing to restore.
namespace {

constexpr char MAGIC[8] = {'O', 'S', 'S', 'T', '1', '\n', 0, 0};
const char *DIR_ = "/sdcard/state";
const char *SNAP = "/sdcard/state/snap.bin";
const char *TMP = "/sdcard/state/snap.tmp";
const char *OLD = "/sdcard/state/snap.old";
constexpr uint32_t EVERY_MS = 60 * 1000;

enum : uint32_t { T_NODES = 1, T_DETS, T_TARGETS, T_STATS, T_METRICS, T_DRONES, T_WD, T_TRAILS };

struct DetsHdr { int32_t ndets, detHead; uint32_t maxDetId; };
struct DronesHdr { int32_t ndrones; uint32_t droneSeq; };

uint8_t *buf = nullptr;   // PSRAM, the whole snapshot
size_t cap = 0;
SemaphoreHandle_t saveLock;
uint32_t savedVer = 0, savedWd = 0, savedMs = 0;
size_t savedBytes = 0;
char st[48] = "not saved";

size_t put(size_t at, uint32_t tag, const void *a, size_t la, const void *b = nullptr, size_t lb = 0) {
    uint32_t h[2] = {tag, (uint32_t)(la + lb)};
    memcpy(buf + at, h, 8);
    memcpy(buf + at + 8, a, la);
    if (b) memcpy(buf + at + 8 + la, b, lb);
    return at + 8 + la + lb;
}

bool alloc() {
    if (buf) return true;
    cap = 8 + 8 * 8 + sizeof(int32_t) + sizeof(Node) * MAX_NODES + sizeof(DetsHdr) + sizeof(Det) * MAX_DETS +
          sizeof(int32_t) + sizeof(Target) * MAX_TARGETS + sizeof(Stats) + sizeof(Metrics) + sizeof(DronesHdr) +
          sizeof(Drone) * MAX_DRONES + 12 + sizeof(WdDev) * WD_MAX + sizeof(int32_t) + 64 * 1024;
    buf = (uint8_t *)ps_malloc(cap);
    return buf != nullptr;
}

// Copy the model into buf under the lock (a few ms of memcpy); the card write is after.
size_t snapshot() {
    model_lock();
    size_t at = 8;
    memcpy(buf, MAGIC, 8);
    int32_t nn = M->nnodes;
    at = put(at, T_NODES, &nn, 4, M->nodes, sizeof(Node) * MAX_NODES);
    DetsHdr dh = {M->ndets, M->detHead, M->maxDetId};
    at = put(at, T_DETS, &dh, sizeof dh, M->dets, sizeof(Det) * MAX_DETS);
    int32_t nt = M->ntargets;
    at = put(at, T_TARGETS, &nt, 4, M->targets, sizeof(Target) * MAX_TARGETS);
    at = put(at, T_STATS, &M->stats, sizeof(Stats));
    at = put(at, T_METRICS, &M->metrics, sizeof(Metrics));
    DronesHdr rh = {M->ndrones, M->droneSeq};
    at = put(at, T_DRONES, &rh, sizeof rh, M->drones, sizeof(Drone) * MAX_DRONES);
    size_t w = wd_snapshot(buf + at + 8, cap - at - 8);
    if (w) {
        uint32_t h[2] = {T_WD, (uint32_t)w};
        memcpy(buf + at, h, 8);
        at += 8 + w;
    }
    void *tr;
    int *ntr;
    size_t tl = node_trails_raw(&tr, &ntr);
    if (tl) {
        int32_t n = *ntr;
        at = put(at, T_TRAILS, &n, 4, tr, tl);
    }
    savedVer = M->version;
    savedWd = wd_version();
    model_unlock();
    return at;
}

bool write_file(size_t len) {
    mkdir(DIR_, 0775);
    FILE *f = fopen(TMP, "wb");
    if (!f) return false;
    bool ok = fwrite(buf, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (!ok) return remove(TMP), false;
    remove(OLD);
    rename(SNAP, OLD);
    if (rename(TMP, SNAP)) return false;
    remove(OLD);
    return true;
}

void restore(const uint8_t *p, size_t len) {
    size_t at = 8;
    int got = 0;
    model_lock();
    while (at + 8 <= len) {
        uint32_t h[2];
        memcpy(h, p + at, 8);
        const uint8_t *d = p + at + 8;
        size_t l = h[1];
        if (at + 8 + l > len) break;
        at += 8 + l;
        switch (h[0]) {
        case T_NODES:
            if (l != 4 + sizeof(Node) * MAX_NODES) continue;
            memcpy(&M->nnodes, d, 4);
            memcpy(M->nodes, d + 4, l - 4);
            M->nnodes = constrain(M->nnodes, 0, MAX_NODES);
            for (int i = 0; i < M->nnodes; i++) {   // last known, not live: the feed says who is up
                Node &n = M->nodes[i];
                n.online = false;
                n.ota[0] = 0;
                n.otaPct = 0;
            }
            break;
        case T_DETS: {
            if (l != sizeof(DetsHdr) + sizeof(Det) * MAX_DETS) continue;
            DetsHdr dh;
            memcpy(&dh, d, sizeof dh);
            if (dh.ndets < 0 || dh.ndets > MAX_DETS || dh.detHead < 0 || dh.detHead >= MAX_DETS) continue;
            memcpy(M->dets, d + sizeof dh, l - sizeof dh);
            M->ndets = dh.ndets, M->detHead = dh.detHead, M->maxDetId = dh.maxDetId;
            for (int i = 0; i < MAX_DETS; i++) M->dets[i].isNew = false;
            break;
        }
        case T_TARGETS:
            if (l != 4 + sizeof(Target) * MAX_TARGETS) continue;
            memcpy(&M->ntargets, d, 4);
            memcpy(M->targets, d + 4, l - 4);
            M->ntargets = constrain(M->ntargets, 0, MAX_TARGETS);
            break;
        case T_STATS:
            if (l != sizeof(Stats)) continue;
            memcpy(&M->stats, d, l);
            break;
        case T_METRICS:
            if (l != sizeof(Metrics)) continue;
            memcpy(&M->metrics, d, l);
            break;
        case T_DRONES: {
            if (l != sizeof(DronesHdr) + sizeof(Drone) * MAX_DRONES) continue;
            DronesHdr rh;
            memcpy(&rh, d, sizeof rh);
            if (rh.ndrones < 0 || rh.ndrones > MAX_DRONES) continue;
            memcpy(M->drones, d + sizeof rh, l - sizeof rh);
            M->ndrones = rh.ndrones, M->droneSeq = rh.droneSeq;
            break;
        }
        case T_WD:
            if (!wd_restore(d, l)) continue;
            break;
        case T_TRAILS: {
            void *tr;
            int *ntr;
            size_t tl = node_trails_raw(&tr, &ntr);
            if (!tl || l != 4 + tl) continue;
            int32_t n;
            memcpy(&n, d, 4);
            memcpy(tr, d + 4, tl);
            *ntr = constrain(n, 0, MAX_NODES);
            break;
        }
        default:
            continue;
        }
        got++;
    }
    M->version++;
    savedVer = M->version;
    savedWd = wd_version();
    model_unlock();
    Serial.printf("store: restored %d parts from the card (%u KB)\n", got, (unsigned)(len / 1024));
}

bool load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = len > 8 && (size_t)len <= cap && fread(buf, 1, len, f) == (size_t)len && !memcmp(buf, MAGIC, 8);
    fclose(f);
    if (ok) restore(buf, len);
    return ok;
}

void saver(void *) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (millis() - savedMs < EVERY_MS) continue;
        if (M->version == savedVer && wd_version() == savedWd) continue;
        store_save();
    }
}

}  // namespace

void store_load() {
    if (!saveLock) saveLock = xSemaphoreCreateMutex();
    if (!sd_ok() || !alloc()) return;
    if (!load(SNAP) && !load(OLD)) Serial.println("store: nothing saved on the card yet");
    savedMs = millis();
}

void store_start() {
    if (!saveLock) saveLock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCoreWithCaps(saver, "store", 6144, nullptr, 1, nullptr, 0, MALLOC_CAP_SPIRAM);
}

void store_save() {
    if (!saveLock || !sd_ok() || !alloc()) return;
    xSemaphoreTake(saveLock, portMAX_DELAY);
    uint32_t t0 = millis();
    size_t len = snapshot();
    bool ok = write_file(len);
    savedMs = millis();
    if (ok) savedBytes = len;
    else savedVer = savedWd = 0;   // try again on the next round
    snprintf(st, sizeof st, ok ? "saved %u KB in %u ms" : "save FAILED (%u KB, %u ms)", (unsigned)(len / 1024),
             (unsigned)(millis() - t0));
    xSemaphoreGive(saveLock);
}

const char *store_state() { return st; }

// ---------------------------------------------------------------- device records
// The dashboard's full record of a device (/api/devices/<mac>?flat=1: decoded intel rows,
// names, group, companions), kept on the card as it came: the detail page shows the same
// thing with no dashboard. /sdcard/intel/<first byte>/<12 hex>.json, 256 folders so no
// folder gets long.
namespace {
bool intel_path(const char *mac, char *out, size_t cap, bool dir) {
    char h[13];
    int k = 0;
    for (const char *p = mac; *p && k < 12; p++)
        if (isxdigit((unsigned char)*p)) h[k++] = toupper(*p);
    if (k != 12) return false;
    h[12] = 0;
    if (dir) snprintf(out, cap, "/sdcard/intel/%.2s", h);
    else snprintf(out, cap, "/sdcard/intel/%.2s/%s.json", h, h);
    return true;
}
}  // namespace

void intel_put(const char *mac, JsonDocument &doc) {
    char p[48];
    if (!sd_ok() || !intel_path(mac, p, sizeof p, true)) return;
    size_t n = measureJson(doc);
    char *b = (char *)ps_malloc(n + 1);
    if (!b) return;
    serializeJson(doc, b, n + 1);
    mkdir("/sdcard/intel", 0775);
    mkdir(p, 0775);
    intel_path(mac, p, sizeof p, false);
    if (FILE *f = fopen(p, "wb")) {
        bool ok = fwrite(b, 1, n, f) == n;
        if (fclose(f) || !ok) remove(p);
    }
    free(b);
}

bool intel_get(const char *mac, JsonDocument &doc) {
    char p[48];
    if (!sd_ok() || !intel_path(mac, p, sizeof p, false)) return false;
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = n > 0 && n < 64 * 1024 ? (char *)ps_malloc(n) : nullptr;
    bool ok = b && fread(b, 1, n, f) == (size_t)n && !deserializeJson(doc, b, n);
    fclose(f);
    free(b);
    return ok;
}

long intel_age(const char *mac) {
    char p[48];
    struct stat st;
    if (!sd_ok() || !intel_path(mac, p, sizeof p, false) || stat(p, &st)) return -1;
    time_t now = time(nullptr);
    return now > st.st_mtime ? (long)(now - st.st_mtime) : 0;
}
