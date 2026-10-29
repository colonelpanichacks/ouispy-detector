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
    f.mode = PPA_TRANS_MODE_BLOCKING;
    ppa_do_fill(fill, &f);
}

void gfx_tile(uint16_t *buf, const uint16_t *tile, int u0, int v0, int uw, int vh, int px, int py, bool ccw, float scale) {
    if (uw <= 0 || vh <= 0) return;
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
    s.mode = PPA_TRANS_MODE_BLOCKING;
    ppa_do_scale_rotate_mirror(srm, &s);
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
