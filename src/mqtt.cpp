#include "mqtt.h"

MQTTConfig mqttCfg;
bool mqttConnected = false;
unsigned long lastDetectionTime = 0;
bool detectionActive = false;
static WiFiClient* mqttTcp = nullptr;
static unsigned long lastReconnect = 0;

void mqtt_loadConfig() {
    Preferences p;
    p.begin("ouispy-mq", true);
    strlcpy(mqttCfg.sta_ssid, p.getString("ssid", "").c_str(), sizeof(mqttCfg.sta_ssid));
    strlcpy(mqttCfg.sta_pass, p.getString("pass", "").c_str(), sizeof(mqttCfg.sta_pass));
    strlcpy(mqttCfg.broker, p.getString("host", "").c_str(), sizeof(mqttCfg.broker));
    mqttCfg.port = p.getUShort("port", 1883);
    strlcpy(mqttCfg.user, p.getString("user", "ouispy").c_str(), sizeof(mqttCfg.user));
    strlcpy(mqttCfg.pass, p.getString("pw", "ouispy").c_str(), sizeof(mqttCfg.pass));
    strlcpy(mqttCfg.device_id, p.getString("devid", "ouispy").c_str(), sizeof(mqttCfg.device_id));
    strlcpy(mqttCfg.topic, p.getString("topic", "").c_str(), sizeof(mqttCfg.topic));
    mqttCfg.enabled = p.getBool("on", false);
    p.end();

    // Auto-generate topic from device_id if not set
    if (mqttCfg.topic[0] == 0) {
        snprintf(mqttCfg.topic, sizeof(mqttCfg.topic), "%s/detection", mqttCfg.device_id);
    }
}

void mqtt_saveConfig() {
    Preferences p;
    p.begin("ouispy-mq", false);
    p.putString("ssid", mqttCfg.sta_ssid);
    p.putString("pass", mqttCfg.sta_pass);
    p.putString("host", mqttCfg.broker);
    p.putUShort("port", mqttCfg.port);
    p.putString("user", mqttCfg.user);
    p.putString("pw", mqttCfg.pass);
    p.putString("devid", mqttCfg.device_id);
    p.putString("topic", mqttCfg.topic);
    p.putBool("on", mqttCfg.enabled);
    p.end();
}

// Raw MQTT over TCP — no library
static void wr8(uint8_t b) { mqttTcp->write(b); }
static void wr16(uint16_t v) { mqttTcp->write(v >> 8); mqttTcp->write(v & 0xFF); }
static void wrStr(const char* s) { uint16_t len = strlen(s); wr16(len); mqttTcp->write((const uint8_t*)s, len); }
static void wrLen(uint32_t len) {
    do { uint8_t b = len & 0x7F; len >>= 7; if (len) b |= 0x80; mqttTcp->write(b); } while (len);
}

void mqtt_connect() {
    if (!mqttCfg.enabled || mqttCfg.broker[0] == 0) return;
    if (mqttTcp == nullptr) mqttTcp = new WiFiClient();
    if (mqttTcp->connected()) mqttTcp->stop();

    const char* cid = mqttCfg.device_id;
    Serial.printf("MQTT: connecting %s:%d as %s\n", mqttCfg.broker, mqttCfg.port, cid);
    if (!mqttTcp->connect(mqttCfg.broker, mqttCfg.port)) {
        Serial.println("MQTT: TCP failed");
        mqttConnected = false;
        return;
    }

    uint32_t cidLen = strlen(cid);
    uint32_t remain = 10 + 2 + cidLen;
    uint8_t flags = 0x02;
    if (mqttCfg.user[0]) { flags |= 0x80; remain += 2 + strlen(mqttCfg.user); }
    if (mqttCfg.pass[0]) { flags |= 0x40; remain += 2 + strlen(mqttCfg.pass); }

    wr8(0x10); wrLen(remain);
    wrStr("MQTT"); wr8(0x04); wr8(flags); wr16(120); // 120s keepalive
    wrStr(cid);
    if (mqttCfg.user[0]) wrStr(mqttCfg.user);
    if (mqttCfg.pass[0]) wrStr(mqttCfg.pass);
    mqttTcp->flush();

    unsigned long t = millis();
    while (!mqttTcp->available() && millis() - t < 3000) delay(10);
    if (mqttTcp->available() >= 4) {
        uint8_t buf[4]; mqttTcp->read(buf, 4);
        mqttConnected = (buf[0] == 0x20 && buf[3] == 0x00);
    } else {
        mqttConnected = false;
    }
    Serial.println(mqttConnected ? "MQTT: connected!" : "MQTT: failed");

    // Publish HA MQTT discovery — unique per device_id
    if (mqttConnected) {
        char disco[512];
        char discoTopic[128];
        snprintf(discoTopic, sizeof(discoTopic), "homeassistant/sensor/%s/detection/config", cid);
        snprintf(disco, sizeof(disco),
            "{\"name\":\"%s Detection\","
            "\"state_topic\":\"%s\","
            "\"value_template\":\"{{ value_json.mac }}\","
            "\"json_attributes_topic\":\"%s\","
            "\"unique_id\":\"%s_detection\","
            "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\",\"manufacturer\":\"Colonel Panic\",\"model\":\"OUI Spy Detector\"}}",
            cid, mqttCfg.topic, mqttCfg.topic, cid, cid, cid);
        mqtt_publish(discoTopic, disco);
        // Set initial state to "online"
        mqtt_publish(mqttCfg.topic, "{\"mac\":\"online\",\"alias\":\"\",\"rssi\":0}");
        Serial.printf("MQTT: HA discovery published for %s\n", cid);
    }
}

void mqtt_publish(const char* topic, const char* payload) {
    if (!mqttConnected || !mqttTcp || !mqttTcp->connected()) { mqttConnected = false; return; }
    uint16_t tlen = strlen(topic);
    uint32_t plen = strlen(payload);
    wr8(0x30); wrLen(2 + tlen + plen);
    wrStr(topic);
    mqttTcp->write((const uint8_t*)payload, plen);
    mqttTcp->flush();
}

static void mqtt_ping() {
    if (!mqttConnected || !mqttTcp || !mqttTcp->connected()) { mqttConnected = false; return; }
    wr8(0xC0); wr8(0x00); mqttTcp->flush();
}

void mqtt_loop(unsigned long now) {
    if (!mqttCfg.enabled) return;

    // Reconnect WiFi if it dropped
    if (WiFi.status() != WL_CONNECTED) {
        mqttConnected = false;
        if (now - lastReconnect >= 15000) {
            lastReconnect = now;
            WiFi.reconnect();
            Serial.println("MQTT: WiFi reconnecting...");
        }
        return;
    }

    if (mqttConnected) {
        // Check TCP is still alive
        if (mqttTcp && !mqttTcp->connected()) {
            mqttConnected = false;
            return;
        }
        if (now - lastReconnect >= 20000) { lastReconnect = now; mqtt_ping(); }
        // Clear detection after 10s of silence — creates blips in HA history
        if (detectionActive && now - lastDetectionTime >= 10000) {
            mqtt_publish(mqttCfg.topic, "{\"mac\":\"idle\",\"alias\":\"\",\"rssi\":0}");
            detectionActive = false;
        }
    } else {
        if (now - lastReconnect >= 10000) { lastReconnect = now; mqtt_connect(); }
    }
}
