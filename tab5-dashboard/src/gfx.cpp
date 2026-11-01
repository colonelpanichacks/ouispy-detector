#include "gfx.h"
#include <driver/ppa.h>
#include <esp_cache.h>
#include <esp_async_memcpy.h>
#include <freertos/semphr.h>

namespace {
constexpr int PW = 720, PH = 1280;
constexpr size_t FRAME = (size_t)PW * PH * 2;
ppa_client_handle_t srm = nullptr, fill = nullptr;
bool ok = false;
async_memcpy_handle_t mcp = nullptr;   // AXI GDMA: bulk copies (the PPA's SRM path is slow for plain copies)
SemaphoreHandle_t done = nullptr;
constexpr size_t CHUNK = 65536;
// A PPA job that never finishes (seen while driving: an SRM tile blit with the map moving
// under it) used to block the UI forever, holding the model lock, until the task watchdog
// restarted the device. Every job now waits at most this long; one that times out retires
// the PPA for good and the CPU draws from then on (slower, but the device keeps running).
constexpr TickType_t JOB_WAIT = pdMS_TO_TICKS(250);
SemaphoreHandle_t jobDone = nullptr;
uint32_t stalls = 0;

bool IRAM_ATTR on_job(ppa_client_handle_t, ppa_event_data_t *, void *) {
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(jobDone, &woke);
    return woke == pdTRUE;
}

bool job_wait(esp_err_t queued) {
    if (queued != ESP_OK) return false;
    if (xSemaphoreTake(jobDone, JOB_WAIT) == pdTRUE) return true;
    ok = false, stalls++;
    Serial.println("gfx: PPA job timed out; drawing on the CPU from now on");
    return false;
}

bool IRAM_ATTR on_copied(async_memcpy_handle_t, async_memcpy_event_t *, void *last) {
    BaseType_t woke = pdFALSE;
    if (last) xSemaphoreGiveFromISR(done, &woke);
    return woke == pdTRUE;
}
}

bool gfx_init() {
    ppa_client_config_t c = {};
    c.oper_type = PPA_OPERATION_SRM;
    c.max_pending_trans_num = 1;
    if (ppa_register_client(&c, &srm) != ESP_OK) return false;
    c.oper_type = PPA_OPERATION_FILL;
    if (ppa_register_client(&c, &fill) != ESP_OK) return false;
    jobDone = xSemaphoreCreateBinary();
    ppa_event_callbacks_t cb = {};
    cb.on_trans_done = on_job;
    if (ppa_client_register_event_callbacks(srm, &cb) != ESP_OK || ppa_client_register_event_callbacks(fill, &cb) != ESP_OK)
        return false;
    async_memcpy_config_t mc = ASYNC_MEMCPY_DEFAULT_CONFIG();
    mc.backlog = FRAME / CHUNK + 4;
    mc.dma_burst_size = 64;
    // The AXI-GDMA copy (19 ms instead of 30) crashed the device within seconds of boot:
    // left off until that is understood; the push is a CPU memcpy.
    (void)mc;
    mcp = nullptr;
    done = xSemaphoreCreateBinary();
    ok = true;
    return true;
}

bool gfx_ok() { return ok; }
void gfx_retire() { ok = false; }
uint32_t gfx_stalls() { return stalls; }

void gfx_cpu_to_mem(void *buf, size_t n) {
    esp_cache_msync(buf, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void gfx_mem_to_cpu(void *buf, size_t n) {
    esp_cache_msync(buf, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void gfx_fill(uint16_t *buf, int px, int py, int pw, int ph, uint16_t col) {
    if (px < 0) pw += px, px = 0;
    if (py < 0) ph += py, py = 0;
    if (px + pw > PW) pw = PW - px;
    if (py + ph > PH) ph = PH - py;
    if (pw <= 0 || ph <= 0) return;
    if (ok) {
        ppa_fill_oper_config_t f = {};
        f.out.buffer = buf;
        f.out.buffer_size = FRAME;
        f.out.pic_w = PW;
        f.out.pic_h = PH;
        f.out.block_offset_x = px;
        f.out.block_offset_y = py;
        f.out.fill_cm = PPA_FILL_COLOR_MODE_RGB565;
        f.fill_block_w = pw;
        f.fill_block_h = ph;
        f.fill_argb_color.r = ((col >> 11) & 31) * 255 / 31;
        f.fill_argb_color.g = ((col >> 5) & 63) * 255 / 63;
        f.fill_argb_color.b = (col & 31) * 255 / 31;
        f.mode = PPA_TRANS_MODE_NON_BLOCKING;
        if (job_wait(ppa_do_fill(fill, &f))) return;
    }
    for (int y = py; y < py + ph; y++) {   // the CPU's fill
        uint16_t *row = buf + (size_t)y * PW + px;
        for (int x = 0; x < pw; x++) row[x] = col;
    }
}

// The PPA's tile blit done by the CPU, nearest pixel: the block scaled to ws x hs, then
// turned a quarter counterclockwise (ccw) or clockwise onto the panel at (px, py).
void cpu_tile(uint16_t *buf, const uint16_t *tile, int u0, int v0, int uw, int vh, int px, int py, bool ccw, float scale) {
    int ws = (int)(uw * scale), hs = (int)(vh * scale);   // the scaled block, before turning
    int ow = hs, oh = ws;                                  // on the panel
    static int16_t col[PW], row[PH];                       // by panel column / row
    int xa = max(0, px), xb = min(PW, px + ow), ya = max(0, py), yb = min(PH, py + oh);
    if (xa >= xb || ya >= yb) return;
    // ccw: panel (ox, oy) shows scaled (ws-1-oy, ox); clockwise: (oy, hs-1-ox).
    for (int x = xa; x < xb; x++) {
        int ox = x - px;
        col[x] = v0 + min(vh - 1, (int)((ccw ? ox : hs - 1 - ox) / scale));
    }
    for (int y = ya; y < yb; y++) {
        int oy = y - py;
        row[y] = u0 + min(uw - 1, (int)((ccw ? ws - 1 - oy : oy) / scale));
    }
    for (int y = ya; y < yb; y++) {
        uint16_t *out = buf + (size_t)y * PW;
        int u = row[y];
        for (int x = xa; x < xb; x++) out[x] = tile[col[x] * 256 + u];
    }
}

void gfx_tile(uint16_t *buf, const uint16_t *tile, int u0, int v0, int uw, int vh, int px, int py, bool ccw, float scale) {
    if (uw <= 0 || vh <= 0) return;
    if (!ok) {
        cpu_tile(buf, tile, u0, v0, uw, vh, px, py, ccw, scale);
        return;
    }
    ppa_srm_oper_config_t s = {};
    s.in.buffer = tile;
    s.in.pic_w = 256;
    s.in.pic_h = 256;
    s.in.block_w = uw;
    s.in.block_h = vh;
    s.in.block_offset_x = u0;
    s.in.block_offset_y = v0;
    s.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    s.out.buffer = buf;
    s.out.buffer_size = FRAME;
    s.out.pic_w = PW;
    s.out.pic_h = PH;
    s.out.block_offset_x = px;
    s.out.block_offset_y = py;
    s.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    s.rotation_angle = ccw ? PPA_SRM_ROTATION_ANGLE_90 : PPA_SRM_ROTATION_ANGLE_270;
    s.scale_x = scale;
    s.scale_y = scale;
    s.mode = PPA_TRANS_MODE_NON_BLOCKING;
    if (!job_wait(ppa_do_scale_rotate_mirror(srm, &s))) cpu_tile(buf, tile, u0, v0, uw, vh, px, py, ccw, scale);
}

void gfx_push(const uint16_t *canvas, uint16_t *fb) {
    if (mcp) {
        // 64 KB DMA transfers back to back; the last one signals completion.
        size_t off = 0;
        bool sent = true;
        while (off < FRAME && sent) {
            size_t n = FRAME - off < CHUNK ? FRAME - off : CHUNK;
            bool last = off + n >= FRAME;
            sent = esp_async_memcpy(mcp, (uint8_t *)fb + off, (uint8_t *)canvas + off, n, on_copied, last ? (void *)1 : nullptr) == ESP_OK;
            off += n;
        }
        if (sent) {
            xSemaphoreTake(done, pdMS_TO_TICKS(100));
            return;
        }
        xSemaphoreTake(done, pdMS_TO_TICKS(100));   // whatever was queued finishes first
    }
    memcpy(fb, canvas, FRAME);
    gfx_cpu_to_mem(fb, FRAME);
}
