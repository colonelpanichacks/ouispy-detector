// Map tiles for the MAP view: 256x256 tiles served by the dashboard (/tiles/<layer>/z/x/y,
// plain HTTP on the LAN, cached on the Mac), decoded once and kept in PSRAM.
#pragma once
#include <Arduino.h>
#include <M5GFX.h>

constexpr int TILE = 256;
constexpr int NLAYERS = 4;
extern const char *LAYER_IDS[NLAYERS];     // dashboard layer ids
extern const char *LAYER_NAMES[NLAYERS];

void maptiles_start();
// The decoded tile, transposed (px[u * TILE + v] = column u, row v), or nullptr (it is
// then requested). Call with the model lock held.
const uint16_t *maptile(uint8_t layer, int z, int x, int y);
const uint16_t *maptile_peek(uint8_t layer, int z, int x, int y);   // cached only, no request
void maptiles_clear_requests();   // the view moved: drop requests that are now off screen
uint32_t maptiles_cached();
