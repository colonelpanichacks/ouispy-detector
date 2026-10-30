#include "sfx.h"
#include "settings.h"
#include <M5Unified.h>

namespace {
struct Note {
    uint16_t hz;   // 0 = rest
    uint16_t ms;
};
// Our own little tunes, chip style: quick, high, unmistakable apart.
const Note DETECT[] = {{1319, 55}, {1760, 55}, {2637, 110}};                                   // up-chirp
const Note DRONE[] = {{880, 90}, {0, 25}, {1175, 90}, {0, 25}, {880, 90}, {0, 25}, {1568, 160}};   // warble, then high
const Note NODE_DOWN[] = {{784, 90}, {587, 90}, {392, 200}};                                   // falling
const Note NODE_UP[] = {{392, 70}, {587, 70}, {784, 130}};                                     // rising
const Note TAP[] = {{2400, 35}};
struct Tune {
    const Note *n;
    uint8_t len;
    const char *name;
};
const Tune TUNES[SFX_COUNT] = {
    {DETECT, 3, "detection"}, {DRONE, 7, "drone"}, {NODE_DOWN, 3, "node down"}, {NODE_UP, 3, "node back"}, {TAP, 1, "tap"},
};
const Tune *cur = nullptr;
uint8_t idx = 0;
uint32_t dueMs = 0;
}  // namespace

void sfx_volume() { M5.Speaker.setVolume(S.vol * 255 / 100); }

void sfx_play(Sfx s) {
    if (!S.sound || s >= SFX_COUNT) return;
    cur = &TUNES[s];
    idx = 0;
    dueMs = millis();
    sfx_loop();
}

void sfx_tick(int freq, int ms) {
    if (!S.sound || cur) return;   // a jingle in progress wins
    M5.Speaker.tone(freq, ms);
}

void sfx_loop() {
    if (!cur || (int32_t)(millis() - dueMs) < 0) return;
    if (idx >= cur->len) {
        cur = nullptr;
        return;
    }
    const Note &n = cur->n[idx++];
    if (n.hz) M5.Speaker.tone(n.hz, n.ms);
    dueMs = millis() + n.ms + 8;
}

const char *sfx_name(Sfx s) { return s < SFX_COUNT ? TUNES[s].name : ""; }
