#pragma once
#include <WiFi.h>
#include <Preferences.h>

// Operating mode — defined here so both main.cpp and mqtt.cpp can reference it
enum OperatingMode {
    CONFIG_MODE,
    SCANNING_MODE
};
extern OperatingMode currentMode;

// True while scanning with WiFi filters installed: the radio time-slices
// between promiscuous sweeps and BLE scans, and the main loop brings STA up
// in bounded report windows during BLE phases (mqttReportTick). mqtt_loop()
// must skip its usual WiFi supervision/reconnect then — it would fight the
// radio. BLE-only scanning keeps STA up permanently and leaves this false.
extern bool g_mqttRadioTimeslice;

struct MQTTConfig {
    char sta_ssid[33];
    char sta_pass[64];
    char broker[65];
    uint16_t port;
    char user[65];
    char pass[65];
    char topic[129];
    char device_id[33];
    bool enabled;
};

extern MQTTConfig mqttCfg;
extern bool mqttConnected;
extern unsigned long lastDetectionTime;
extern bool detectionActive;

void mqtt_loadConfig();
void mqtt_saveConfig();
void mqtt_connect();
// Publishes with retain=1 by default so HA never sees "unknown" after
// a broker or HA restart. For an idle sensor an unretained publish
// means the entity stays unknown until the next detection.
void mqtt_publish(const char* topic, const char* payload);
// Explicit retained publish (identical to mqtt_publish today; separate
// symbol so future non-retained call sites can be added without
// changing semantics of existing detection publishes).
void mqtt_publish_retained(const char* topic, const char* payload);
void mqtt_loop(unsigned long now);
// Cleanly tear down the TCP connection: sends a DISCONNECT control packet
// (0xE0) if connected, then stops the socket. Called by the main loop's
// time-sliced report flush after publishing, before the device drops STA
// mode — the broker sees an orderly disconnect rather than a dropped socket.
void mqtt_disconnect();
