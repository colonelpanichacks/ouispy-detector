#include "maptiles.h"
#include "model.h"
#include "app.h"
#include "net.h"
#include "gfx.h"
#include "sdcard.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/platform.h>

// Keyless, checked by their pixels: Esri imagery, and Esri's dark gray canvas with its
// labels composited in by the dashboard (or without). CARTO now wants an API key.
const char *LAYER_IDS[NLAYERS] = {"esri-sat", "esri-dark", "esri-darkgray"};
const char *LAYER_NAMES[NLAYERS] = {"SATELLITE", "DARK", "DARK, NO LABELS"};
const int8_t LAYER_MAX[NLAYERS] = {19, 16, 16};   // the dashboard tile proxy's limits (overzoom beyond)

namespace {

constexpr int CACHE_N = 120;    // 120 x 128 KB = 15 MB of the 32 MB PSRAM
constexpr int WANT_N = 96;
constexpr size_t BUF_N = 160 * 1024;

// A tile is a row-major 256x256 rgb565 picture; the PPA rotates and scales it onto the
// portrait framebuffer when it is drawn.
struct Slot {
    uint16_t *img;
    uint8_t layer;
    int8_t z;
    int32_t x, y;
    uint32_t used;
};
Slot cache[CACHE_N];
uint32_t tick = 0;

struct Want { uint8_t layer; int8_t z; int32_t x, y; };
Want want[WANT_N];
int nwant = 0;
constexpr int LOADERS = 1;   // one download at a time: each TCP burst lands in internal RAM (64 KB window), and two plus the socket starved the Wi-Fi driver (sdio_read assert)
constexpr size_t HEAP_FLOOR = 48 * 1024;   // internal RAM a download leaves for Wi-Fi and the rest
TaskHandle_t loaderTask[LOADERS] = {};

int slot_of(uint8_t layer, int z, int x, int y) {
    for (int i = 0; i < CACHE_N; i++)
        if (cache[i].img && cache[i].layer == layer && cache[i].z == z && cache[i].x == x && cache[i].y == y)
            return i;
    return -1;
}

// The same public servers the dashboard's tile proxy uses (tiles.py), for when the
// dashboard isn't reachable: any Wi-Fi with internet (a phone hotspot) still gets maps.
// The dark map comes without its label layer this way (the dashboard composites those).
const char *DIRECT[NLAYERS] = {
    "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/%d/%ld/%ld",
    "https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Dark_Gray_Base/MapServer/tile/%d/%ld/%ld",
    "https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Dark_Gray_Base/MapServer/tile/%d/%ld/%ld",
};
constexpr size_t PLACEHOLDER_UNDER = 10000;   // Esri's "no imagery here yet" tile (satellite): never kept
uint32_t dashFailMs = 0;                      // the dashboard last failed a tile: go direct for a while

// TLS buffers (~40 KB a connection) in PSRAM, not the internal RAM the Wi-Fi link lives on.
void *tls_calloc(size_t n, size_t sz) {
    void *p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_calloc(n, sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t read_body(HTTPClient &h, uint8_t *buf) {
    size_t n = 0;
    int len = h.getSize();
    WiFiClient *s = h.getStreamPtr();
    uint32_t t0 = millis();
    while (n < BUF_N && millis() - t0 < 8000) {
        int a = s->available();
        if (a > 0) {
            n += s->readBytes(buf + n, min((size_t)a, BUF_N - n));
            if (len > 0 && (int)n >= len) break;
        } else if (!h.connected()) {
            break;
        } else {
            delay(2);
        }
    }
    return n;
}

size_t from_dashboard(const Want &w, uint8_t *buf) {
    String base = net_base();
    if (!base.length()) return 0;
    char path[96];
    snprintf(path, sizeof path, "/tiles/%s/%d/%ld/%ld", LAYER_IDS[w.layer], w.z, (long)w.x, (long)w.y);
    WiFiClient c;
    HTTPClient h;
    h.setTimeout(8000);
    h.setConnectTimeout(3000);
    if (!h.begin(c, base + path)) return 0;
    size_t n = h.GET() == 200 ? read_body(h, buf) : 0;
    h.end();
    return n;
}

size_t from_internet(const Want &w, uint8_t *buf, bool &keep) {
    static WiFiClientSecure *c = nullptr;
    if (!c) {
        c = new WiFiClientSecure();
        c->setInsecure();   // public map pictures, nothing sent: no certificate store to carry
    }
    char url[160];
    snprintf(url, sizeof url, DIRECT[w.layer], w.z, (long)w.y, (long)w.x);
    HTTPClient h;
    h.setTimeout(10000);
    h.setConnectTimeout(5000);
    h.setReuse(true);
    h.setUserAgent("OUI-SPY-Mesh-Tab5/1.0 (field map)");
    if (!h.begin(*c, url)) return 0;
    size_t n = h.GET() == 200 ? read_body(h, buf) : 0;
    h.end();
    keep = !(w.layer == 0 && n < PLACEHOLDER_UNDER);
    return n;
}

// One tile from the dashboard, else straight from the internet; its size (0 = failed).
// keep: worth putting in the card's cache.
size_t download(const Want &w, uint8_t *buf, bool &keep) {
    keep = true;
    if (WiFi.status() != WL_CONNECTED) return 0;
    size_t n = 0;
    if (!dashFailMs || millis() - dashFailMs > 30000) {
        n = from_dashboard(w, buf);
        dashFailMs = n ? 0 : millis();
    }
    if (!n) n = from_internet(w, buf, keep);
    return n;
}

void loader(void *) {
    uint8_t *buf = (uint8_t *)ps_malloc(BUF_N);
    M5Canvas *dec = new M5Canvas();   // decode target, reused
    dec->setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    dec->setPsram(true);
    if (!buf || !dec->createSprite(TILE, TILE)) vTaskDelete(nullptr);
    for (;;) {
        Want w;
        bool have = false;
        model_lock();
        if (nwant) {
            w = want[--nwant];   // newest request first: what is on screen now
            have = slot_of(w.layer, w.z, w.x, w.y) < 0;
        }
        model_unlock();
        if (!have || A.otaActive) {   // an update owns the Wi-Fi while it runs
            vTaskDelay(pdMS_TO_TICKS(nwant ? 1 : 40));
            continue;
        }
        // Internal RAM is small (Wi-Fi, TCP and every task stack live there) and running
        // it dry crashes the whole device: wait for room instead of downloading into it.
        // The card first (no Wi-Fi needed, and no internal RAM: SD DMA reads into PSRAM),
        // the dashboard for what it doesn't have.
        // Then the card's cache of tiles fetched before; a download is kept there too.
        const char *lid = LAYER_IDS[w.layer];
        int src = 0;   // 0 pack, 1 cache, 2 download
        bool keep = true;
        size_t n = sd_tile(lid, w.z, w.x, w.y, buf, BUF_N);
        if (!n && (n = sd_cache_get(lid, w.z, w.x, w.y, buf, BUF_N))) src = 1;
        if (!n) {
            src = 2;
            net_bulk_begin();
            while (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < HEAP_FLOOR) vTaskDelay(pdMS_TO_TICKS(100));
            n = download(w, buf, keep);
            net_bulk_end();
        }
        if (!n) {
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        dec->fillSprite(0);
        bool png = n > 4 && buf[0] == 0x89 && buf[1] == 'P';
        if (!(png ? dec->drawPng(buf, n, 0, 0) : dec->drawJpg(buf, n, 0, 0))) {
            if (src == 1) sd_cache_drop(lid, w.z, w.x, w.y);   // a bad copy: fetch it again next time
            continue;
        }
        if (src == 2 && keep) sd_cache_put(lid, w.z, w.x, w.y, buf, n);
        // Row-major, cache-line aligned, written out of the CPU cache: the PPA reads it by DMA.
        uint16_t *t = (uint16_t *)heap_caps_aligned_alloc(128, TILE * TILE * 2, MALLOC_CAP_SPIRAM);
        if (!t) continue;
        memcpy(t, dec->getBuffer(), TILE * TILE * 2);
        gfx_cpu_to_mem(t, TILE * TILE * 2);
        model_lock();
        if (slot_of(w.layer, w.z, w.x, w.y) >= 0) {   // another loader got it first
            model_unlock();
            free(t);
            continue;
        }
        int s = 0;
        for (int i = 0; i < CACHE_N; i++) {   // a free slot, else the least recently drawn
            if (!cache[i].img) { s = i; break; }
            if (cache[i].used < cache[s].used) s = i;
        }
        if (cache[s].img) free(cache[s].img);
        cache[s] = {t, w.layer, w.z, w.x, w.y, ++tick};
        M->version++;   // redraw with the new tile
        model_unlock();
    }
}

}  // namespace

void maptiles_start() {
    mbedtls_platform_set_calloc_free(tls_calloc, free);
    // 24 KB each (PNG decoding, HTTP and a TLS handshake), in PSRAM: these tasks never touch flash, and
    // internal RAM is the scarce one.
    for (int i = 0; i < LOADERS; i++)
        xTaskCreatePinnedToCoreWithCaps(loader, "tiles", 24576, nullptr, 1, &loaderTask[i], 0, MALLOC_CAP_SPIRAM);
}

const uint16_t *maptile_peek(uint8_t layer, int z, int x, int y) {
    int s = slot_of(layer, z, x, y);
    return s >= 0 ? cache[s].img : nullptr;
}

const uint16_t *maptile(uint8_t layer, int z, int x, int y) {
    int s = slot_of(layer, z, x, y);
    if (s >= 0) {
        cache[s].used = ++tick;
        return cache[s].img;
    }
    for (int i = 0; i < nwant; i++)
        if (want[i].layer == layer && want[i].z == z && want[i].x == x && want[i].y == y) return nullptr;
    if (nwant == WANT_N) {   // full: drop the oldest request
        memmove(want, want + 1, sizeof(Want) * (WANT_N - 1));
        nwant--;
    }
    want[nwant++] = {layer, (int8_t)z, x, y};
    return nullptr;
}

void maptiles_clear_requests() { nwant = 0; }

uint32_t maptiles_stack_min() {
    uint32_t m = 0xFFFFFFFF;
    for (auto t : loaderTask)
        if (t) m = min(m, (uint32_t)uxTaskGetStackHighWaterMark(t));
    return m;
}

uint32_t maptiles_cached() {
    uint32_t n = 0;
    for (auto &c : cache) n += c.img != nullptr;
    return n;
}
