// Wardriving (like WiGLE), the `wardrive` build only (pio run -e wardrive): every WiFi
// access point and every Bluetooth LE device heard, no filters. Detector and Sky-Spy
// nodes don't have it. Its channel plan and raw mode are set at runtime (console "wd",
// or "rwd <node|all> ..." over the mesh from the dashboard / Tab5). Each one is printed on serial as
//   WD {"t":"wifi","mac":..,"ssid":..,"ch":6,"rssi":-70,"sec":"WPA2",...}
//   WD {"t":"ble","mac":..,"name":..,"rssi":-80,"rnd":1,"mfr":76,...}
// for the Tab5 (or any host): it stamps each sighting with its GPS fix, plots it and logs
// WiGLE CSV. A device is printed when first heard and again every WD_REPEAT_MS while it
// is still around (or sooner when it is a lot stronger), so a moving receiver gets fresh
// positions to place it by. Nothing of this goes over the mesh.
//
// Raw mode (console "wd raw on"): every management frame and every BLE advert as hex,
//   RAW {"t":"wifi","st":8,"ch":6,"rssi":-70,"a1":..,"a2":..,"a3":..,"len":180,"hex":".."}
//   RAW {"t":"ble","mac":..,"rssi":-80,"rnd":1,"hex":".."}
// for reading devices over the air. Off at boot.
#pragma once
#include <Arduino.h>

#ifdef WARDRIVE_NODE
constexpr bool wdOn() { return true; }   // this build is a wardriver
#else
constexpr bool wdOn() { return false; }
#endif
// Wardriving switched on (console "wd on|off", or from the dashboard / Tab5; kept in NVS).
// Off: the radios keep scanning but nothing is collected or printed.
bool wdActive();

// Producers: radio callbacks (WiFi driver task, NimBLE host task). Heap-free.
// f = 802.11 frame from the MAC header, len without the FCS.
void wdWifiFrame(const uint8_t* f, int len, uint8_t subtype, int8_t rssi, uint8_t rxChan);
void wdBleAdvert(const uint8_t mac[6], bool randomAddr, int8_t rssi, const char* name,
                 bool hasMfr, uint16_t mfr, uint16_t appearance, const uint8_t* adv, int advLen);

// loop(): print what the callbacks queued.
void wdLoop();

// Console: "wd" (state), "wd on|off", "wd raw on|off", "wd ch all|6|1-6|1,6,11" (kept in NVS).
// Returns true when the line was ours.
bool wdConsole(const String& verb, const String& arg);

// MESH_CMD_WARDRIVE payload: u8 on (0/1, 255 keep), u8 raw (0/1, 255 keep), u16 le channel
// mask (bit n = channel n, 0 keep). Returns false on a bad payload or a node that is
// not a wardriver.
bool wdApplyMesh(const uint8_t* p, uint16_t len);
// Build that payload from console words ("on", "raw on", "ch 1-6"); false if invalid.
bool wdMeshPayload(const String& words, uint8_t out[4]);

// The channel plan for the WiFi sweep: one channel (it stays there), a range or a list
// (hopped high to low), or all 13. Several wardrivers can split the band between them.
uint8_t wdFirstChannel();
uint8_t wdNextChannel(uint8_t cur);
void wdChannelsText(char* out, size_t cap);   // "1-11", "6", "1,6,11"

// Telemetry: devices seen since boot, per radio.
uint32_t wdWifiCount();
uint32_t wdBleCount();
