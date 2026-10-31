// Map tiles for the MAP view: 256x256 tiles served by the dashboard (/tiles/<layer>/z/x/y,
// plain HTTP on the LAN, cached on the Mac), decoded once and kept in PSRAM.
#pragma once
#include <Arduino.h>
#include <M5GFX.h>

constexpr int TILE = 256;
constexpr int NLAYERS = 3;   // dark and satellite only (no light maps), all keyless
extern const char *LAYER_IDS[NLAYERS];     // dashboard layer ids
extern const char *LAYER_NAMES[NLAYERS];
extern const int8_t LAYER_MAX[NLAYERS];    // each source's last real tile zoom

void maptiles_start();
// The decoded tile (row-major 256x256 rgb565), or nullptr (it is then requested). Call
// with the model lock held.
const uint16_t *maptile(uint8_t layer, int z, int x, int y);
const uint16_t *maptile_peek(uint8_t layer, int z, int x, int y);   // cached only, no request
void maptiles_clear_requests();   // the view moved: drop requests that are now off screen
uint32_t maptiles_cached();
void maptiles_timing(char *out, size_t cap);   // where tile time goes, for /info
uint32_t maptiles_stack_min();   // bytes of stack never used by the loaders (health)
