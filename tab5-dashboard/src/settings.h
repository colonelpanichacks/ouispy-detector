// Device settings, kept in NVS (namespace "tab5dash"). Nothing here is compiled in:
// they are typed on the Settings screen or sent over serial ("set ssid ...").
#pragma once
#include <Arduino.h>

enum Source : uint8_t { SRC_AUTO = 0, SRC_NETWORK = 1, SRC_USB = 2 };

struct Settings {
    // AUTO: a node on USB-A is used (and bridged to the dashboard when it's reachable),
    // otherwise the dashboard over Wi-Fi. NETWORK ignores USB. USB = no network feed at
    // all (Wi-Fi only joins, if configured, for OTA).
    uint8_t source = SRC_AUTO;
    String ssid, pass;
    String host;          // dashboard address: IP or name.local
    uint16_t port = 5077;
    String token;         // only needed off the dashboard's trusted networks
    String otaPass;       // empty = OTA off
    String tz = "EST5EDT,M3.2.0,M11.1.0";
    String hostname = "ouispy-tab5";
    bool flip = false;
    uint8_t bright = 70;  // %
    bool sound = true;
    String muted;         // comma-separated MACs whose alerts are silenced
};

extern Settings S;

void settings_load();
void settings_save();
bool settings_ready();    // enough to run without the setup screen
bool wifi_wanted();       // join Wi-Fi at all
// Serial console: "set <key> <value>", "show", "reboot", "clear". Returns true if handled.
bool settings_serial_line(const String &line);
