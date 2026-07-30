#include "mqtt.h"

MQTTConfig mqttCfg;
bool mqttConnected = false;
unsigned long lastDetectionTime = 0;
bool detectionActive = false;

static WiFiClient* mqttTcp = nullptr;

// Separate cadence timers — previously one shared `lastReconnect` variable
// served all three roles and could shadow one interval with another.
static unsigned long lastWifiRetry = 0;
static unsigned long lastMqttRetry = 0;
static unsigned long lastPing      = 0;

// WiFi hard-reset backstop: after N soft `WiFi.reconnect()` attempts
// still fail, tear down and re-init the stack from scratch. Some
// ESP32 WiFi drops recover only via disconnect/begin, not reconnect.
static uint8_t wifiSoftRetries = 0;
static const uint8_t WIFI_HARD_RESET_AFTER = 5;

// Per-boot random suffix appended to `device_id` to form the MQTT
// client-ID. Prevents session-collision when the ESP32 reconnects
// before the broker has cleaned up its previous session with the
// same client-ID (a common cause of infinite-reconnect loops after
// a WiFi drop).
static char mqttClientId[64] = {0};

// Availability topic (LWT publishes `offline`, we publish `online`
// once CONNACK arrives — both retained so HA reflects state fast).
static char availTopic[160] = {0};

static const char* AVAIL_ONLINE  = "online";
static const char* AVAIL_OFFLINE = "offline";

void mqtt_loadConfig() {
    Preferences p;
    p.begin("ouispy-mq", true);
    strlcpy(mqttCfg.sta_ssid, p.getString("ssid", "").c_str(), sizeof(mqttCfg.sta_ssid));
    strlcpy(mqttCfg.sta_pass, p.getString("pass", "").c_str(), sizeof(mqttCfg.sta_pass));
    strlcpy(mqttCfg.broker,   p.getString("host", "").c_str(), sizeof(mqttCfg.broker));
    mqttCfg.port = p.getUShort("port", 1883);
    strlcpy(mqttCfg.user, p.getString("user", "ouispy").c_str(), sizeof(mqttCfg.user));
    strlcpy(mqttCfg.pass, p.getString("pw",   "ouispy").c_str(), sizeof(mqttCfg.pass));
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
    p.putString("ssid",  mqttCfg.sta_ssid);
    p.putString("pass",  mqttCfg.sta_pass);
    p.putString("host",  mqttCfg.broker);
    p.putUShort("port",  mqttCfg.port);
    p.putString("user",  mqttCfg.user);
    p.putString("pw",    mqttCfg.pass);
    p.putString("devid", mqttCfg.device_id);
    p.putString("topic", mqttCfg.topic);
    p.putBool  ("on",    mqttCfg.enabled);
    p.end();
}

// Raw MQTT over TCP — no library
static void wr8(uint8_t b)         { mqttTcp->write(b); }
static void wr16(uint16_t v)       { mqttTcp->write(v >> 8); mqttTcp->write(v & 0xFF); }
static void wrStr(const char* s)   { uint16_t len = strlen(s); wr16(len); mqttTcp->write((const uint8_t*)s, len); }
static void wrLen(uint32_t len) {
    do { uint8_t b = len & 0x7F; len >>= 7; if (len) b |= 0x80; mqttTcp->write(b); } while (len);
}

// Build the availability topic and per-session client-ID exactly once
// per connect so downstream code (discovery, LWT, publishes) can reuse.
// device_id stays untouched — that's what HA groups the device by.
static void buildIdentity() {
    // Availability topic: <device_id>/availability
    snprintf(availTopic, sizeof(availTopic), "%s/availability", mqttCfg.device_id);

    // Client-ID: <device_id>-<mac6> so reconnects can't collide with
    // a still-lingering broker session for the same ID.
    String mac = WiFi.macAddress();  // "AA:BB:CC:DD:EE:FF"
    mac.replace(":", "");
    String suffix = mac.substring(mac.length() - 6);  // last 3 bytes as hex
    snprintf(mqttClientId, sizeof(mqttClientId), "%s-%s",
             mqttCfg.device_id, suffix.c_str());
}

// Length of the PUBLISH availability payload we'll set as LWT.
static uint16_t lenU16(const char* s) { return (uint16_t)strlen(s); }

void mqtt_connect() {
    if (!mqttCfg.enabled || mqttCfg.broker[0] == 0) return;
    if (mqttTcp == nullptr) mqttTcp = new WiFiClient();
    if (mqttTcp->connected()) mqttTcp->stop();

    buildIdentity();
    Serial.printf("MQTT: connecting %s:%d as %s (LWT %s)\n",
                  mqttCfg.broker, mqttCfg.port, mqttClientId, availTopic);
    if (!mqttTcp->connect(mqttCfg.broker, mqttCfg.port)) {
        Serial.println("MQTT: TCP failed");
        mqttConnected = false;
        return;
    }

    uint32_t cidLen = strlen(mqttClientId);

    // CONNECT flags:
    //   0x02 clean session
    //   0x04 will flag         (NEW — enable LWT)
    //   0x20 will retain       (NEW — HA can see availability state on subscribe)
    //   0x40 password (if any)
    //   0x80 username (if any)
    uint8_t flags = 0x02 | 0x04 | 0x20;
    uint32_t remain = 10 + 2 + cidLen;
    remain += 2 + lenU16(availTopic);       // will topic
    remain += 2 + lenU16(AVAIL_OFFLINE);    // will message
    if (mqttCfg.user[0]) { flags |= 0x80; remain += 2 + strlen(mqttCfg.user); }
    if (mqttCfg.pass[0]) { flags |= 0x40; remain += 2 + strlen(mqttCfg.pass); }

    wr8(0x10); wrLen(remain);
    wrStr("MQTT"); wr8(0x04); wr8(flags); wr16(120);  // 120s keepalive
    wrStr(mqttClientId);
    wrStr(availTopic);
    wrStr(AVAIL_OFFLINE);
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

    if (mqttConnected) {
        // Tell HA we're online — retained so subscribers see it immediately.
        mqtt_publish_retained(availTopic, AVAIL_ONLINE);

        // HA MQTT discovery — unique per device_id.
        //   - availability_topic: HA knows online/offline in real time
        //   - expire_after 60s: entity goes unavailable if no messages
        //   - device block: consistent grouping across multiple sensors
        char disco[768];
        char discoTopic[128];
        const char* cid = mqttCfg.device_id;
        snprintf(discoTopic, sizeof(discoTopic),
                 "homeassistant/sensor/%s/detection/config", cid);
        snprintf(disco, sizeof(disco),
            "{\"name\":\"%s Detection\","
            "\"state_topic\":\"%s\","
            "\"value_template\":\"{{ value_json.mac }}\","
            "\"json_attributes_topic\":\"%s\","
            "\"unique_id\":\"%s_detection\","
            "\"availability_topic\":\"%s\","
            "\"payload_available\":\"%s\","
            "\"payload_not_available\":\"%s\","
            "\"expire_after\":60,"
            "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
                        "\"manufacturer\":\"Colonel Panic\","
                        "\"model\":\"OUI Spy Detector\"}}",
            cid, mqttCfg.topic, mqttCfg.topic, cid,
            availTopic, AVAIL_ONLINE, AVAIL_OFFLINE,
            cid, cid);
        // Discovery config must be retained so HA sees it on any restart.
        mqtt_publish_retained(discoTopic, disco);

        // Retained initial state so HA has something to display.
        mqtt_publish_retained(mqttCfg.topic,
            "{\"mac\":\"online\",\"alias\":\"\",\"rssi\":0}");
        Serial.printf("MQTT: HA discovery published for %s\n", cid);

        // A successful reconnect resets the WiFi hard-reset backstop.
        wifiSoftRetries = 0;
    }
}

// Internal publish core — set `retain` to control the retain bit.
static void publish_core(const char* topic, const char* payload, bool retain) {
    if (!mqttConnected || !mqttTcp || !mqttTcp->connected()) { mqttConnected = false; return; }
    uint16_t tlen = strlen(topic);
    uint32_t plen = strlen(payload);
    uint8_t header = 0x30 | (retain ? 0x01 : 0x00);
    wr8(header); wrLen(2 + tlen + plen);
    wrStr(topic);
    mqttTcp->write((const uint8_t*)payload, plen);
    mqttTcp->flush();
}

void mqtt_publish(const char* topic, const char* payload) {
    // Detection publishes default to RETAINED. Without this, HA sees
    // "unknown" every time it (or the broker) restarts until the next
    // detection lands — for an idle sensor that's minutes to hours.
    publish_core(topic, payload, true);
}

void mqtt_publish_retained(const char* topic, const char* payload) {
    publish_core(topic, payload, true);
}

static void mqtt_ping() {
    if (!mqttConnected || !mqttTcp || !mqttTcp->connected()) { mqttConnected = false; return; }
    wr8(0xC0); wr8(0x00); mqttTcp->flush();
}

// Full WiFi stack teardown when soft reconnect gives up. Some ESP32
// WiFi drops (radio firmware wedges) recover only via disconnect + begin.
static void hardResetWifi() {
    Serial.println("MQTT: WiFi hard reset (disconnect + begin)");
    WiFi.disconnect(true, true);
    delay(200);
    WiFi.mode(WIFI_STA);
    if (mqttCfg.sta_ssid[0]) WiFi.begin(mqttCfg.sta_ssid, mqttCfg.sta_pass);
    else                     WiFi.begin();
    wifiSoftRetries = 0;
}

void mqtt_loop(unsigned long now) {
    if (!mqttCfg.enabled) return;

    // WiFi supervision: soft reconnect first, hard-reset backstop after N.
    if (WiFi.status() != WL_CONNECTED) {
        mqttConnected = false;
        if (now - lastWifiRetry >= 15000) {
            lastWifiRetry = now;
            if (wifiSoftRetries < WIFI_HARD_RESET_AFTER) {
                WiFi.reconnect();
                wifiSoftRetries++;
                Serial.printf("MQTT: WiFi reconnecting (attempt %u/%u)...\n",
                              wifiSoftRetries, WIFI_HARD_RESET_AFTER);
            } else {
                hardResetWifi();
            }
        }
        return;
    }

    if (mqttConnected) {
        // TCP dropped underneath us?
        if (mqttTcp && !mqttTcp->connected()) {
            mqttConnected = false;
            return;
        }
        // Keepalive ping every 20s (well under the 120s broker window).
        if (now - lastPing >= 20000) {
            lastPing = now;
            mqtt_ping();
        }
        // Signal idle after 10s of detection silence — clears the entity's
        // active state without going unavailable (availability topic still
        // says online, expire_after resets on this message).
        if (detectionActive && now - lastDetectionTime >= 10000) {
            mqtt_publish(mqttCfg.topic, "{\"mac\":\"idle\",\"alias\":\"\",\"rssi\":0}");
            detectionActive = false;
        }
    } else {
        if (now - lastMqttRetry >= 10000) {
            lastMqttRetry = now;
            mqtt_connect();
        }
    }
}
