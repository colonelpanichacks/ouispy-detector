// What the Tab5 knows survives a reboot: detections, targets, nodes, drones, metrics,
// wardriving and the GPS trails, saved to the microSD card (/sdcard/state/snap.bin) once a
// minute when something changed, and read back at boot before any feed starts.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

void store_load();    // at boot, after sd_begin(); nothing happens without a card
void store_start();   // the saver task
void store_save();    // now (before an update reboots it); any task
const char *store_state();   // "saved 12 s ago, 1.3 MB" for /info

// The dashboard's record of each device, kept on the card (see store.cpp).
void intel_put(const char *mac, JsonDocument &doc);
bool intel_get(const char *mac, JsonDocument &doc);
long intel_age(const char *mac);   // s since it was saved, -1 = not on the card
