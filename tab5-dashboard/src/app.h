// What the person is looking at: view, selection, search, filters. Owned by the UI task;
// net.cpp only reads the metrics query fields (range / node / dom).
#pragma once
#include <Arduino.h>
#include <atomic>

enum View : uint8_t { V_OVERVIEW, V_FLEET, V_LIVE, V_TARGETS, V_MAP, V_DRONES, V_METRICS, V_COUNT };

constexpr uint32_t RANGES[] = {3600, 6 * 3600, 86400, 7 * 86400};
constexpr const char *RANGE_NAMES[] = {"1H", "6H", "24H", "7D"};
constexpr int NRANGES = 4;

struct App {
    View view = V_OVERVIEW;
    int sel[V_COUNT] = {};      // selected row per view
    int top[V_COUNT] = {};      // first visible row per view
    char search[32] = "";
    bool searching = false;     // typing into the search box
    uint8_t dom = 0;            // 0 all, 1 ble, 2 wifi
    char node[7] = "";          // node filter, "" = all
    std::atomic<uint8_t> range{2};        // metrics range index (24H)
    std::atomic<uint32_t> metricsGen{1};  // bump to make net refetch metrics now
    // map
    double mapLat = 0, mapLon = 0;
    uint8_t mapZoom = 17, mapLayer = 0;
    float mapZoomF = 17;        // displayed zoom, eases toward mapZoom
    uint8_t mapPrevLayer = 0;   // shown under the new layer until its tiles arrive
    bool mapInit = false;       // centred once on the fleet
    bool mapFollow = false;     // keep the newest live drone centred
    bool triOn = true;          // map: devices at the dashboard's triangulated fix, lines to the nodes
    bool droneTracks = true;    // map: drone flight tracks
    bool pilotOn = true;        // map: pilots, their track and the line to their drone
    bool layerMenu = false;     // the map layer dropdown is open
    char nodeEdit[7] = "";      // node settings panel open for this node id (long press)
    bool editNode = false;      // typing that node's alias
    uint32_t rebootArm = 0;     // reboot needs a second tap within 3 s
    bool editDrone = false;     // typing the selected drone's alias
    // detail pages
    bool detail = false;        // device detail open (M->detail.mac)
    bool editAlias = false;     // typing the device's alias
    bool newGroup = false;      // typing a new group's name
    char editBuf[40] = "";
    bool settings = false;
    bool autoSetup = false;     // setup opened at boot for lack of settings
    bool help = false;
    bool otaActive = false;
    uint8_t otaPct = 0;
    char toast[64] = "";
    uint32_t toastUntil = 0;
};

extern App A;

void toast(const char *fmt, ...);
