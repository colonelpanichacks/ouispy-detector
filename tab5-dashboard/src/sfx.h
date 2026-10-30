// Alert sounds: short square-wave jingles on the Tab5 speaker (our own tunes), played a note
// at a time from the UI loop so nothing blocks. Which events sound is in Settings (SOUND
// page in setup); S.sound is the master switch.
#pragma once
#include <Arduino.h>

enum Sfx : uint8_t { SFX_DETECT, SFX_DRONE, SFX_NODE_DOWN, SFX_NODE_UP, SFX_TAP, SFX_COUNT };

void sfx_play(Sfx s);             // starts it now (replaces whatever is playing)
void sfx_tick(int freq, int ms);  // one short note now (the proximity beep)
void sfx_loop();                  // call often: plays the next note when it is due
void sfx_volume();                // apply S.vol
const char *sfx_name(Sfx s);
