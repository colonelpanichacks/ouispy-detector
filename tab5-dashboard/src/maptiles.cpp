#include "maptiles.h"
#include "model.h"
#include "app.h"
#include "net.h"
#include "gfx.h"
#include <HTTPClient.h>
#include <WiFi.h>

const char *LAYER_IDS[NLAYERS] = {"esri-sat", "carto-dark", "esri-darkgray"};
const char *LAYER_NAMES[NLAYERS] = {"SATELLITE", "DARK", "DARK GRAY"};
const int8_t LAYER_MAX[NLAYERS] = {19, 20, 16};   // the dashboard tile proxy's limits

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

// Download one tile from the dashboard into buf; returns its size (0 = failed).
size_t download(const Want &w, uint8_t *buf) {
    String base = net_base();
    if (!base.length() || WiFi.status() != WL_CONNECTED) return 0;
    char path[96];
    snprintf(path, sizeof path, "/tiles/%s/%d/%ld/%ld", LAYER_IDS[w.layer], w.z, (long)w.x, (long)w.y);
    WiFiClient c;
    HTTPClient h;
    h.setTimeout(8000);
    h.setConnectTimeout(3000);
    if (!h.begin(c, base + path)) return 0;
    size_t n = 0;
    if (h.GET() == 200) {
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
    }
    h.end();
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
        while (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < HEAP_FLOOR) vTaskDelay(pdMS_TO_TICKS(100));
        size_t n = download(w, buf);
        if (!n) {
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        dec->fillSprite(0);
        bool png = n > 4 && buf[0] == 0x89 && buf[1] == 'P';
        if (!(png ? dec->drawPng(buf, n, 0, 0) : dec->drawJpg(buf, n, 0, 0))) continue;
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
    // 16 KB each (PNG decoding plus HTTP), in PSRAM: these tasks never touch flash, and
    // internal RAM is the scarce one.
    for (int i = 0; i < LOADERS; i++)
        xTaskCreatePinnedToCoreWithCaps(loader, "tiles", 16384, nullptr, 1, &loaderTask[i], 0, MALLOC_CAP_SPIRAM);
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
