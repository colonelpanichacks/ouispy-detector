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
    bool enabled;
};

extern MQTTConfig mqttCfg;
extern bool mqttConnected;
extern unsigned long lastDetectionTime;
extern bool detectionActive;

void mqtt_loadConfig();
void mqtt_saveConfig();
void mqtt_connect();
void mqtt_publish(const char* topic, const char* payload);
void mqtt_loop(unsigned long now);
