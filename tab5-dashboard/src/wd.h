// Wardriving on the Tab5: every access point and Bluetooth device a wardriver heard,
// placed where it was loudest. Fed by the wardriver on our USB (its WD lines, stamped
// here with our GPS fix), by the dashboard's live "wd" events and, for the map's
// History view, by /api/wardrive. Kept in PSRAM, merged per MAC + radio.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

constexpr int WD_MAX = 8000;

struct WdDev {
    uint8_t mac[6];
    uint8_t kind;          // 1 WiFi AP, 2 BLE
    int8_t rssi;           // strongest heard
    uint8_t sec;           // 0 open, 1 WEP, 2 WPA, 3 WPA2, 4 WPA3, 5 WPA2/WPA3, 255 n/a
    uint8_t ch;
    bool session;          // heard this session
    bool placed;           // has a position
    float lat, lon;        // where it was loudest
    double last;           // server time (s)
    uint32_t seen;
    char name[24];         // SSID / BLE name
};

// o: a WD line's JSON (t, mac, ssid/name, rssi, sec, ch, lat, lon) or a dashboard device
// (kind, mac, name, rssi, sec, ch, lat, lon, last, seen, now). Call under the model lock.
void wd_ingest(JsonObjectConst o, bool session);
int wd_count();
const WdDev &wd_at(int i);
uint32_t wd_session_count(uint8_t kind);   // 0 = both
uint32_t wd_version();                     // bumps on every change (redraw)
void wd_new_session();                     // everything so far becomes history
