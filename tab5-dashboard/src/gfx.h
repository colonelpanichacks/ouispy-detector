// Hardware pixel work on the ESP32-P4's PPA (pixel processing accelerator): fills, tile
// blits with rotation and scaling, and the frame push, all by DMA. The CPU copies through
// PSRAM at ~40-70 MB/s (a full 1280x720 frame cost 20-30 ms per pass); the PPA does it in a
// few ms and leaves the CPU free.
//
// Coordinates here are PANEL coordinates (the native 720x1280 portrait framebuffer); ui.cpp
// maps its landscape canvas onto them.
#pragma once
#include <Arduino.h>

bool gfx_init();
bool gfx_ok();

// Fill a panel-space rectangle of `buf` (720x1280 rgb565) with an rgb565 colour.
void gfx_fill(uint16_t *buf, int px, int py, int pw, int ph, uint16_t col);

// Copy block (u0, v0, uw, vh) of a row-major 256x256 rgb565 tile into `buf` at panel
// (px, py), rotated a quarter turn (ccw = true for the canvas' normal orientation, false
// when flipped) and scaled by `scale` (a multiple of 1/16).
void gfx_tile(uint16_t *buf, const uint16_t *tile, int u0, int v0, int uw, int vh, int px, int py, bool ccw, float scale);

// The whole canvas buffer to the display's framebuffer, unrotated.
void gfx_push(const uint16_t *canvas, uint16_t *fb);

// Before the PPA writes into a buffer the CPU has drawn on (write the CPU's cached pixels
// out first), and after (drop stale cached lines so the CPU sees the PPA's pixels).
void gfx_cpu_to_mem(void *buf, size_t n);
void gfx_mem_to_cpu(void *buf, size_t n);
