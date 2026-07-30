#pragma once
#include <WiFi.h>
#include <Preferences.h>

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
