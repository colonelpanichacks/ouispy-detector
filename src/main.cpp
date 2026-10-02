#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <NimBLEUtils.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <vector>
#include <algorithm>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include "mqtt.h"

// ================================
// 802.11 Header Definition
// ================================
// Not exposed in the public Arduino ESP32S3 headers — define it ourselves
// (same layout as the internal ESP-IDF type used by flock-you).
typedef struct __attribute__((packed)) {
    uint16_t frame_ctrl;
    uint16_t duration;
    uint8_t  addr1[6];
    uint8_t  addr2[6];
    uint8_t  addr3[6];
    uint16_t seq_ctrl;
} wifi_ieee80211_mac_hdr_t;

// ================================
// Pin and Buzzer Definitions - Xiao ESP32 S3
// ================================
#define BUZZER_PIN 3   // GPIO3 (D2) for buzzer - good PWM pin on Xiao ESP32 S3
#define BUZZER_FREQ 2000  // Frequency in Hz
#define BUZZER_DUTY 127  // 50% duty cycle for good volume without excessive power draw
#define BEEP_DURATION 200  // Duration of each beep in ms
#define BEEP_PAUSE 50  // Pause between beeps in ms (faster sequence)
#define LED_PIN 21   // GPIO21 for onboard LED (inverted logic)

// The boot melody  - 153 BPM
// 8th=196ms, 16th=98ms, transposed up 2 octaves for buzzer range
struct BootTone { uint16_t freq; uint16_t duration; };
static const BootTone bootMelody[] = {
    {1319, 196}, {1319, 196},      // 8E6 8E6
    {1319, 98}, {1175, 98}, {988, 98},   // 16E6 16D6 16B5
    {1175, 196}, {1175, 196},      // 8D6 8D6
    {1175, 98}, {988, 98}, {880, 98},    // 16D6 16B5 16A5
    {1047, 196}, {1047, 196},      // 8C6 8C6
    {1047, 98}, {988, 98}, {784, 98},    // 16C6 16B5 16G5
    {1175, 98}, {784, 98}, {1175, 98}, {1319, 98}  // 16D6 16G5 16D6 16E6
};
#define MELODY_BOOT_LEN 19

// ================================
// NeoPixel Definitions - Xiao ESP32 S3
// ================================
#define NEOPIXEL_PIN 4   // GPIO4 (D3) for NeoPixel - confirmed safe pin on Xiao ESP32 S3
#define NEOPIXEL_COUNT 1 // Number of NeoPixels (1 for single pixel)
#define NEOPIXEL_BRIGHTNESS 50 // Brightness (0-255)
#define NEOPIXEL_DETECTION_BRIGHTNESS 200 // Brightness during detection (0-255)

// NeoPixel object
Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// NeoPixel state variables
bool detectionMode = false;
unsigned long detectionStartTime = 0;
int detectionFlashCount = 0;

// ================================
// WiFi AP Configuration
// ================================
String AP_SSID = "snoopuntothem";
String AP_PASSWORD = "astheysnoopuntous";


// (OperatingMode enum defined in mqtt.h)

// ================================
// Global Variables
// ================================
OperatingMode currentMode = CONFIG_MODE;
AsyncWebServer server(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;
const IPAddress captivePortalIP(192, 168, 4, 1);
Preferences preferences;
NimBLEScan* pBLEScan;
unsigned long modeSwitchScheduled = 0; // When to switch modes (0 = not scheduled)
unsigned long deviceResetScheduled = 0; // When to reset device (0 = not scheduled)
unsigned long normalRestartScheduled = 0; // When to do normal restart (0 = not scheduled)


// A single `volatile bool newMatchFound` could only hold one detection, so two
// cameras probing in the same 200 ms channel dwell would overwrite each other.
// The BLE callback and the WiFi promiscuous callback both enqueue into this
// small ring; loop() drains it. Entries use fixed char buffers so the producer
// (a driver/ISR-context callback) never touches the heap.
// Filter classification — determines which BLE advert field the matcher
// checks against `identifier`. Values are persisted to NVS; do NOT
// renumber existing entries or old configs will break.
enum FilterType : uint8_t {
    // BLE domain (unchanged)
    FT_MAC_PREFIX      = 0,  // identifier = 6-char OUI (e.g. "985949")
    FT_FULL_MAC        = 1,  // identifier = 12-char MAC
    FT_COMPANY_ID      = 2,  // identifier = 4-char hex "0D53" (BT SIG mfr CID)
    FT_SERVICE_UUID_16 = 3,  // identifier = 4-char hex "FD5F" (BT SIG 16-bit svc UUID)
    FT_NAME_SUBSTRING  = 4,  // identifier = case-insensitive substring
    // Meta/Ray-Ban composite (mfr CID 0x0D53 + svc UUID 0xFD5F in the SAME
    // advert, or a name-substring hit). User-installable via the META preset
    // in the OUI Database and persisted like any other filter — no filter
    // installed means no Meta detection. Kept so existing NVS values 0-4
    // stay stable.
    FT_META_COMPOSITE  = 5,

    // WiFi domain (new) — matched in the promiscuous callback against the
    // source OUI of an 802.11 management frame.
    FT_WIFI_PROBE      = 6,  // WiFi probe request from OUI
    FT_WIFI_BEACON     = 7,  // WiFi beacon from OUI
};

struct DetectionEntry {
    char mac[18];
    char identifier[18];
    char description[64];
    int rssi;
    FilterType matchedType;
    char matchType[8];
    bool wifiDomain;
};

static const int DET_QUEUE_SIZE = 16;

static DetectionEntry detQueue[DET_QUEUE_SIZE];
static volatile uint8_t detQueueHead = 0;  // write index
static volatile uint8_t detQueueTail = 0;  // read index
static volatile uint8_t detQueueCount = 0;  // entries currently queued

// Persistent settings
bool buzzerEnabled = true;
bool ledEnabled = true;
// Device tracking
struct DeviceInfo {
    String macAddress;
    int rssi;
    unsigned long firstSeen;
    unsigned long lastSeen;
    bool inCooldown;
    unsigned long cooldownUntil;
    const char* matchedFilter;
    String filterDescription;  // Store filter description for persistence
    String matchedIdentifier;  // Raw identifier that triggered (e.g. "985949")
    FilterType matchedType = FT_MAC_PREFIX;  // Which of the 5 filter types hit
};



// Helper: does this filter type belong to the WiFi domain? The domain is
// derived from the type — no separate field is stored on TargetFilter.
static inline bool isWifiDomain(FilterType t) { return t >= FT_WIFI_PROBE; }

// Short code shown on the dashboard match-type badge and persisted in
// the session JSON. Kept stable across firmware versions — the UI's
// colour palette is keyed off these exact strings.
static const char* filterTypeCode(FilterType t) {
    switch (t) {
        case FT_MAC_PREFIX:      return "OUI";
        case FT_FULL_MAC:        return "MAC";
        case FT_COMPANY_ID:      return "CID";
        case FT_SERVICE_UUID_16: return "SVC";
        case FT_NAME_SUBSTRING:  return "NAME";
        case FT_META_COMPOSITE:  return "META";
        case FT_WIFI_PROBE:      return "PROBE";
        case FT_WIFI_BEACON:     return "BEACON";
    }
    return "OUI";
}


struct TargetFilter {
    String identifier;
    bool isFullMAC;      // kept for NVS backwards-compat with pre-typed configs
    String description;
    FilterType type = FT_MAC_PREFIX;  // set from isFullMAC on legacy load
};


struct DeviceAlias {
    String macAddress;
    String alias;
};

std::vector<DeviceInfo> devices;
std::vector<TargetFilter> targetFilters;
std::vector<DeviceAlias> deviceAliases;

// Forward declarations
void startScanningMode();
void startConfigMode();
void startDetectionFlash();
class MyAdvertisedDeviceCallbacks;
void enqueueDetection(const String& mac, const String& ident, const String& desc,
                      int rssi, FilterType type, const char* matchType, bool wifi);
void enqueueDetection(const char* mac, const char* ident, const char* desc,
                      int rssi, FilterType type, const char* matchType, bool wifi);

// WiFi promiscuous mode forward declarations
static bool wifiOuiInTargets(const uint8_t oui[3]);
static void rebuildWifiOuiTable();
static void handleWifiMatch(const uint8_t mac[6], uint8_t subtype, int8_t rssi);
static void startWifiSweep();
static void stopWifiSweep();
static void startBleScan();
static void stopBleScan();
static void doStaReport(const String& payload);


// ================================
// Serial Configuration
// ================================
void initializeSerial() {
    Serial.begin(115200);
    delay(100);
}

bool isSerialConnected() {
    return Serial;
}

// ================================
// LED Control Functions (inverted logic for Xiao ESP32-S3)
// ================================
void ledOn() {
    if (ledEnabled) {
        digitalWrite(LED_PIN, LOW);  // LOW = LED ON for Xiao ESP32-S3
    }
}

void ledOff() {
    if (ledEnabled) {
        digitalWrite(LED_PIN, HIGH); // HIGH = LED OFF for Xiao ESP32-S3
    }
}

// ================================
// Buzzer Functions
// ================================
void initializeBuzzer() {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    ledcSetup(0, BUZZER_FREQ, 8);
    ledcAttachPin(BUZZER_PIN, 0);
    
    // Setup LED (inverted logic - HIGH = OFF for Xiao ESP32-S3)
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);
}

void digitalBeep(int duration) {
    unsigned long startTime = millis();
    while (millis() - startTime < duration) {
        digitalWrite(BUZZER_PIN, HIGH);
        delayMicroseconds(250);
        digitalWrite(BUZZER_PIN, LOW);
        delayMicroseconds(250);
    }
}

void singleBeep() {
    if (buzzerEnabled) {
        ledcWrite(0, BUZZER_DUTY);
    }
    ledOn();
    delay(BEEP_DURATION);
    if (buzzerEnabled) {
        ledcWrite(0, 0);
        digitalBeep(BEEP_DURATION);
    }
    ledOff();
}

void threeBeeps() {
    // Start detection flash animation
    startDetectionFlash();
    
    for(int i = 0; i < 3; i++) {
        singleBeep();
        if (i < 2) delay(BEEP_PAUSE);
    }
}

// ================================
// NeoPixel Functions
// ================================
void initializeNeoPixel() {
    strip.begin();
    strip.setBrightness(NEOPIXEL_BRIGHTNESS);
    strip.clear();
    strip.show();
}

// Convert HSV to RGB
uint32_t hsvToRgb(uint16_t h, uint8_t s, uint8_t v) {
    uint8_t r, g, b;
    
    if (s == 0) {
        r = g = b = v;
    } else {
        uint8_t region = h / 43;
        uint8_t remainder = (h - (region * 43)) * 6;
        
        uint8_t p = (v * (255 - s)) >> 8;
        uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
        uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
        
        switch (region) {
            case 0: r = v; g = t; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = t; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = t; g = p; b = v; break;
            default: r = v; g = p; b = q; break;
        }
    }
    
    return strip.Color(r, g, b);
}

// Normal pink breathing animation
void normalBreathingAnimation() {
    static unsigned long lastUpdate = 0;
    static float brightness = 0.0;
    static bool increasing = true;
    
    unsigned long currentTime = millis();
    
    // Update every 20ms for smooth animation
    if (currentTime - lastUpdate >= 20) {
        lastUpdate = currentTime;
        
        // Update brightness (breathing effect)
        if (increasing) {
            brightness += 0.02;
            if (brightness >= 1.0) {
                brightness = 1.0;
                increasing = false;
            }
        } else {
            brightness -= 0.02;
            if (brightness <= 0.1) {
                brightness = 0.1;
                increasing = true;
            }
        }
        
        // Pink color (hue 300) with breathing brightness
        uint32_t color = hsvToRgb(300, 255, (uint8_t)(NEOPIXEL_BRIGHTNESS * brightness));
        strip.setPixelColor(0, color);
        strip.show();
    }
}

// Detection flash animation synchronized with beeps
void detectionFlashAnimation() {
    unsigned long currentTime = millis();
    unsigned long elapsed = currentTime - detectionStartTime;
    
    // Calculate which flash we're on based on elapsed time
    int currentFlash = (elapsed / (BEEP_DURATION + BEEP_PAUSE)) % 3;
    unsigned long flashProgress = elapsed % (BEEP_DURATION + BEEP_PAUSE);
    
    // Determine color based on flash number
    uint16_t hue;
    if (currentFlash == 0) {
        hue = 240; // Blue
    } else if (currentFlash == 1) {
        hue = 300; // Pink
    } else {
        hue = 270; // Purple
    }
    
    // Flash brightness - bright during beep, dim during pause
    uint8_t brightness;
    if (flashProgress < BEEP_DURATION) {
        // During beep - bright flash
        brightness = NEOPIXEL_DETECTION_BRIGHTNESS;
    } else {
        // During pause - dim
        brightness = NEOPIXEL_BRIGHTNESS / 4;
    }
    
    // Set color
    uint32_t color = hsvToRgb(hue, 255, brightness);
    strip.setPixelColor(0, color);
    strip.show();
    
    // End detection mode after 3 flashes (same as threeBeeps)
    if (elapsed >= (BEEP_DURATION + BEEP_PAUSE) * 3) {
        detectionMode = false;
    }
}

// Main animation function
void updateNeoPixelAnimation() {
    if (detectionMode) {
        detectionFlashAnimation();
    } else {
        normalBreathingAnimation();
    }
}

// Set NeoPixel to a specific color
void setNeoPixelColor(uint8_t r, uint8_t g, uint8_t b) {
    strip.setPixelColor(0, strip.Color(r, g, b));
    strip.show();
}

// Turn off NeoPixel
void turnOffNeoPixel() {
    strip.clear();
    strip.show();
}

// Start detection flash animation
void startDetectionFlash() {
    detectionMode = true;
    detectionStartTime = millis();
}

void twoBeeps() {
    for(int i = 0; i < 2; i++) {
        singleBeep();
        if (i < 1) delay(BEEP_PAUSE);
    }
}

void playBoot() {
    if (!buzzerEnabled) return;
    for (int i = 0; i < MELODY_BOOT_LEN; i++) {
        ledcSetup(0, bootMelody[i].freq, 8);
        ledcWrite(0, BUZZER_DUTY);
        ledOn();
        delay(bootMelody[i].duration);
        ledcWrite(0, 0);
        ledOff();
    }
    // Reset to original frequency for future detection beeps
    ledcSetup(0, BUZZER_FREQ, 8);
}

void ascendingBeeps() {
    // Two fast ascending beeps to indicate "ready to scan"
    int frequencies[] = {1900, 2200}; // Close melodic interval, not octave
    int fastPause = 100; // Faster than normal beeps
    
    for (int i = 0; i < 2; i++) {
        if (buzzerEnabled) {
            ledcSetup(0, frequencies[i], 8);
            ledcWrite(0, BUZZER_DUTY);
        }
        ledOn();
        delay(BEEP_DURATION);
        if (buzzerEnabled) {
            ledcWrite(0, 0);
            digitalBeep(BEEP_DURATION);  // Fallback: always works even if LEDC is broken
        }
        ledOff();
        if (i < 1) delay(fastPause);
    }
    
    // Reset to original frequency for future beeps
    if (buzzerEnabled) {
        ledcSetup(0, BUZZER_FREQ, 8);
    }
}

// ================================
// Configuration Storage Functions
// ================================
void saveConfiguration() {
    preferences.begin("ouispy", false);
    preferences.putInt("filterCount", targetFilters.size());
    preferences.putBool("buzzerEnabled", buzzerEnabled);
    preferences.putBool("ledEnabled", ledEnabled);
    
    for (int i = 0; i < targetFilters.size(); i++) {
        String keyId   = "id_"   + String(i);
        String keyMAC  = "mac_"  + String(i);
        String keyDesc = "desc_" + String(i);
        String keyType = "type_" + String(i);

        preferences.putString(keyId.c_str(),   targetFilters[i].identifier);
        preferences.putBool  (keyMAC.c_str(),  targetFilters[i].isFullMAC);
        preferences.putString(keyDesc.c_str(), targetFilters[i].description);
        preferences.putUChar (keyType.c_str(), (uint8_t)targetFilters[i].type);
    }
    
    preferences.end();
    
    if (isSerialConnected()) {
        Serial.println("Configuration saved to NVS");
    }
}

void loadConfiguration() {
    preferences.begin("ouispy", true);
    int filterCount = preferences.getInt("filterCount", 0);
    buzzerEnabled = preferences.getBool("buzzerEnabled", true);
    ledEnabled = preferences.getBool("ledEnabled", true);
    
    targetFilters.clear();
    
    // Load saved filters (no defaults - start empty)
    if (filterCount > 0) {
        for (int i = 0; i < filterCount; i++) {
            String keyId   = "id_"   + String(i);
            String keyMAC  = "mac_"  + String(i);
            String keyDesc = "desc_" + String(i);
            String keyType = "type_" + String(i);

            TargetFilter filter;
            filter.identifier  = preferences.getString(keyId.c_str(), "");
            filter.isFullMAC   = preferences.getBool  (keyMAC.c_str(), false);
            filter.description = preferences.getString(keyDesc.c_str(), "");

            // Legacy compat: if `type_i` is missing (255 sentinel), derive
            // from the old boolean — old configs are all MAC-based.
            uint8_t rawType = preferences.getUChar(keyType.c_str(), 0xFF);
            if (rawType == 0xFF) {
                filter.type = filter.isFullMAC ? FT_FULL_MAC : FT_MAC_PREFIX;
            } else if (rawType <= FT_WIFI_BEACON) {
                filter.type = (FilterType)rawType;
            } else {
                filter.type = FT_MAC_PREFIX;  // corrupt value, fail safe
            }

            if (filter.identifier.length() > 0) {
                targetFilters.push_back(filter);
            }
        }
    }
    // No default values - form starts empty (placeholder examples remain in HTML)
    
    preferences.end();
}

void loadWiFiCredentials() {
    preferences.begin("ouispy", true);
    AP_SSID = preferences.getString("ap_ssid", "snoopuntothem");
    AP_PASSWORD = preferences.getString("ap_password", "astheysnoopuntous");
    preferences.end();
}

void saveWiFiCredentials() {
    preferences.begin("ouispy", false);
    preferences.putString("ap_ssid", AP_SSID);
    preferences.putString("ap_password", AP_PASSWORD);
    preferences.end();
}

// ================================
// MAC Address Utility Functions
// ================================
void normalizeMACAddress(String& mac) {
    mac.toLowerCase();
    mac.replace("-", ":");
    mac.replace(" ", "");
}

bool isValidMAC(const String& mac) {
    String normalized = mac;
    normalizeMACAddress(normalized);
    
    // Check for valid OUI (8 chars) or full MAC (17 chars)
    if (normalized.length() != 8 && normalized.length() != 17) {
        return false;
    }
    
    // Basic format validation
    for (int i = 0; i < normalized.length(); i++) {
        char c = normalized.charAt(i);
        if (i % 3 == 2) {
            if (c != ':') return false;
        } else {
            if (!isxdigit(c)) return false;
        }
    }
    
    return true;
}

// Case-insensitive substring for FT_NAME_SUBSTRING.
static bool nameContains(const String& haystack, const String& needle) {
    if (needle.length() == 0 || haystack.length() < needle.length()) return false;
    String h = haystack; h.toLowerCase();
    String n = needle;   n.toLowerCase();
    return h.indexOf(n) >= 0;
}

// Normalise a hex-string identifier ("0x0D53", "0d53", "0D 53") to a
// bare lowercase hex string so string compares work.
static String normalizeHexId(const String& in) {
    String s = in;
    s.toLowerCase();
    if (s.startsWith("0x")) s = s.substring(2);
    String out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) out += c;
    }
    return out;
}

// Enqueue a detection into the ring buffer from String objects. Used in the
// BLE callback path. Drops the entry if the queue is full.
void enqueueDetection(const String& mac, const String& ident, const String& desc,
                      int rssi, FilterType type, const char* matchType, bool wifi) {
    if (detQueueCount >= DET_QUEUE_SIZE) return;  // queue full, drop

    int slot = detQueueHead;
    strncpy(detQueue[slot].mac, mac.c_str(), sizeof(detQueue[slot].mac) - 1);
    detQueue[slot].mac[sizeof(detQueue[slot].mac) - 1] = '\0';
    strncpy(detQueue[slot].identifier, ident.c_str(), sizeof(detQueue[slot].identifier) - 1);
    detQueue[slot].identifier[sizeof(detQueue[slot].identifier) - 1] = '\0';
    strncpy(detQueue[slot].description, desc.c_str(), sizeof(detQueue[slot].description) - 1);
    detQueue[slot].description[sizeof(detQueue[slot].description) - 1] = '\0';
    detQueue[slot].rssi = rssi;
    detQueue[slot].matchedType = type;
    strncpy(detQueue[slot].matchType, matchType, sizeof(detQueue[slot].matchType) - 1);
    detQueue[slot].matchType[sizeof(detQueue[slot].matchType) - 1] = '\0';
    detQueue[slot].wifiDomain = wifi;

    detQueueHead = (detQueueHead + 1) % DET_QUEUE_SIZE;
    detQueueCount++;
}

// Enqueue a detection into the ring buffer from char* buffers. Used in the
// WiFi promiscuous callback — no String allocation in driver context.
// Drops the entry if the queue is full.
void enqueueDetection(const char* mac, const char* ident, const char* desc,
                      int rssi, FilterType type, const char* matchType, bool wifi) {
    if (detQueueCount >= DET_QUEUE_SIZE) return;  // queue full, drop

    int slot = detQueueHead;
    strncpy(detQueue[slot].mac, mac, sizeof(detQueue[slot].mac) - 1);
    detQueue[slot].mac[sizeof(detQueue[slot].mac) - 1] = '\0';
    strncpy(detQueue[slot].identifier, ident, sizeof(detQueue[slot].identifier) - 1);
    detQueue[slot].identifier[sizeof(detQueue[slot].identifier) - 1] = '\0';
    strncpy(detQueue[slot].description, desc, sizeof(detQueue[slot].description) - 1);
    detQueue[slot].description[sizeof(detQueue[slot].description) - 1] = '\0';
    detQueue[slot].rssi = rssi;
    detQueue[slot].matchedType = type;
    strncpy(detQueue[slot].matchType, matchType, sizeof(detQueue[slot].matchType) - 1);
    detQueue[slot].matchType[sizeof(detQueue[slot].matchType) - 1] = '\0';
    detQueue[slot].wifiDomain = wifi;

    detQueueHead = (detQueueHead + 1) % DET_QUEUE_SIZE;
    detQueueCount++;
}

// WiFi OUI fast-path table. A small const array of 3-byte OUIs extracted
// from installed FT_WIFI_PROBE and FT_WIFI_BEACON filters, rebuilt whenever
// those filters change. The promiscuous callback checks the frame's source OUI
// against this table before doing the full filter match — saving CPU on the
// common case (99.9% of frames don't match).
static const int WIFI_OUI_TABLE_SIZE = 16;  // max entries
static uint8_t wifiOuiTable[WIFI_OUI_TABLE_SIZE][3];
static int wifiOuiTableCount = 0;

// Rebuild the fast-path OUI table from installed WiFi filters.
static void rebuildWifiOuiTable() {
    wifiOuiTableCount = 0;
    for (const TargetFilter& f : targetFilters) {
        if (!isWifiDomain(f.type)) continue;
        // Identifiers are stored as bare 6-char hex ("D42DC5") — no colons.
        // normalizeMACAddress lowercases; strip colons too in case a legacy
        // colon form slipped in from an old config.
        String id = f.identifier;
        id.replace(":", "");
        id.replace("-", "");
        if (id.length() < 6) continue;  // need at least 6 hex chars for an OUI

        uint8_t oui[3];
        oui[0] = (uint8_t)strtol(id.substring(0, 2).c_str(), NULL, 16);
        oui[1] = (uint8_t)strtol(id.substring(2, 4).c_str(), NULL, 16);
        oui[2] = (uint8_t)strtol(id.substring(4, 6).c_str(), NULL, 16);

        // Check if this OUI is already in the table
        bool exists = false;
        for (int i = 0; i < wifiOuiTableCount; i++) {
            if (wifiOuiTable[i][0] == oui[0] &&
                wifiOuiTable[i][1] == oui[1] &&
                wifiOuiTable[i][2] == oui[2]) {
                exists = true;
                break;
            }
        }
        if (!exists && wifiOuiTableCount < WIFI_OUI_TABLE_SIZE) {
            memcpy(wifiOuiTable[wifiOuiTableCount], oui, 3);
            wifiOuiTableCount++;
        }
    }
}

// Fast OUI pre-check: is this 3-byte OUI in our target table?
static bool wifiOuiInTargets(const uint8_t oui[3]) {
    for (int i = 0; i < wifiOuiTableCount; i++) {
        if (wifiOuiTable[i][0] == oui[0] &&
            wifiOuiTable[i][1] == oui[1] &&
            wifiOuiTable[i][2] == oui[2]) {
            return true;
        }
    }
    return false;
}

// Match WiFi source MAC against FT_WIFI_PROBE / FT_WIFI_BEACON filters.
// Uses fixed-size char buffers — no String allocation in driver callback context.
static bool matchesWifiFilter(const uint8_t mac[6], uint8_t subtype,
                               char desc[64], char ident[18], FilterType& outType) {
    // Format MAC as "AABBCCDDEEFF" (no colons, uppercase) in a local buffer
    char macBuf[13];
    macBuf[0] = '\0';
    for (int i = 0; i < 6; i++) {
        char hi = (mac[i] >> 4) & 0x0F;
        char lo = mac[i] & 0x0F;
        macBuf[i * 2]     = hi > 9 ? 'A' + hi - 10 : '0' + hi;
        macBuf[i * 2 + 1] = lo > 9 ? 'A' + lo - 10 : '0' + lo;
    }
    macBuf[12] = '\0';

    // OUI is first 6 hex chars (bytes 0-2 of MAC, e.g. "AABBCC")
    char oui[7];
    oui[0] = macBuf[0]; oui[1] = macBuf[1]; oui[2] = macBuf[2];
    oui[3] = macBuf[3]; oui[4] = macBuf[4]; oui[5] = macBuf[5];
    oui[6] = '\0';

    for (const TargetFilter& f : targetFilters) {
        if (!isWifiDomain(f.type)) continue;

        bool subtypeMatch = false;
        if (f.type == FT_WIFI_PROBE && subtype == 0x04) subtypeMatch = true;
        else if (f.type == FT_WIFI_BEACON && subtype == 0x08) subtypeMatch = true;
        else continue;

        if (subtypeMatch) {
            const char* fid = f.identifier.c_str();
            size_t flen = strlen(fid);
            bool ouiMatch = (flen <= 6);
            for (size_t k = 0; k < flen && ouiMatch; k++) {
                char fc = fid[k];
                char fcUp = (fc >= 'a' && fc <= 'f') ? (fc - 'a' + 'A') : fc;
                if (fcUp != oui[k]) ouiMatch = false;
            }
            if (ouiMatch) {
                strncpy(desc, f.description.c_str(), 63);
                desc[63] = '\0';
                strncpy(ident, f.identifier.c_str(), 17);
                ident[17] = '\0';
                outType = f.type;
                return true;
            }
        }
    }
    return false;
}

// Forward declaration — defined below; FT_META_COMPOSITE filters defer to it.
bool matchesMetaComposite(NimBLEAdvertisedDevice* dev, const char*& outLabel);

bool matchesTargetFilter(NimBLEAdvertisedDevice* dev, const String& deviceMAC,
                          String& matchedDescription, String& matchedIdentifier,
                          FilterType& outType) {
    String normalizedDeviceMAC = deviceMAC;
    normalizeMACAddress(normalizedDeviceMAC);

    for (const TargetFilter& filter : targetFilters) {
        switch (filter.type) {
            case FT_MAC_PREFIX: {
                String filterID = filter.identifier;
                normalizeMACAddress(filterID);
                if (normalizedDeviceMAC.startsWith(filterID)) {
                    matchedDescription = filter.description;
                    matchedIdentifier  = filter.identifier;
                    outType            = filter.type;
                    return true;
                }
                break;
            }
            case FT_FULL_MAC: {
                String filterID = filter.identifier;
                normalizeMACAddress(filterID);
                if (normalizedDeviceMAC.equals(filterID)) {
                    matchedDescription = filter.description;
                    matchedIdentifier  = filter.identifier;
                    outType            = filter.type;
                    return true;
                }
                break;
            }
            case FT_COMPANY_ID: {
                if (!dev || !dev->haveManufacturerData()) break;
                std::string mfr = dev->getManufacturerData();
                if (mfr.length() < 2) break;
                uint16_t cid = (uint8_t)mfr[0] | ((uint8_t)mfr[1] << 8);
                char cidHex[5];
                snprintf(cidHex, sizeof(cidHex), "%04x", cid);
                if (normalizeHexId(filter.identifier).equals(cidHex)) {
                    matchedDescription = filter.description;
                    matchedIdentifier  = filter.identifier;
                    outType            = filter.type;
                    return true;
                }
                break;
            }
            case FT_SERVICE_UUID_16: {
                if (!dev) break;
                String target = normalizeHexId(filter.identifier);
                for (int i = 0; i < dev->getServiceUUIDCount(); i++) {
                    NimBLEUUID uuid = dev->getServiceUUID(i);
                    String s = uuid.toString().c_str();
                    s.toLowerCase();
                    if (s.length() == 4 && s.equals(target)) {
                        matchedDescription = filter.description;
                        matchedIdentifier  = filter.identifier;
                        outType            = filter.type;
                        return true;
                    }
                    if (s.length() >= 8 && s.substring(4, 8).equals(target)) {
                        matchedDescription = filter.description;
                        matchedIdentifier  = filter.identifier;
                        outType            = filter.type;
                        return true;
                    }
                }
                break;
            }
            case FT_NAME_SUBSTRING: {
                if (!dev || !dev->haveName()) break;
                String name = dev->getName().c_str();
                if (nameContains(name, filter.identifier)) {
                    matchedDescription = filter.description;
                    matchedIdentifier  = filter.identifier;
                    outType            = filter.type;
                    return true;
                }
                break;
            }
            case FT_META_COMPOSITE: {
                // Installed via the META preset; defer to the composite
                // matcher. The label it returns ("META-RAYBAN (mfr+svc)" /
                // "META-RAYBAN (name)") is more informative than the filter
                // description, so it wins.
                const char* metaLabel = nullptr;
                if (matchesMetaComposite(dev, metaLabel)) {
                    matchedDescription = metaLabel;
                    matchedIdentifier  = filter.identifier;
                    outType            = filter.type;
                    return true;
                }
                break;
            }
        }
    }
    return false;
}

// Meta / Ray-Ban composite matcher.
//
// Only runs while a FT_META_COMPOSITE filter is installed (via the META
// preset in the OUI Database) — matchesTargetFilter calls it from that
// case. Does NOT use OUI signals: Meta glasses use RPA (rotating random
// MACs per BT spec), so OUI-based detection is pure noise, and CID-alone
// or UUID-alone filters were false-positive magnets (0xFD5F is advertised
// by phones running Meta apps). This matcher fires when BOTH conditions in
// condition A are present in the same advert, OR when condition B fires:
//   A. mfr data starts with company ID 0x0D53 (Luxottica, little-endian
//      0x53 0x0D) AND service UUID list contains 0xFD5F (Meta).
//   B. complete local name contains "Ray-Ban" / "Wayfarer" / "Oakley Meta"
//      (case-insensitive substring).
//
// A user manually installing 0x0D53, 0xFD5F, or a Luxottica MAC via the
// target config UI still triggers via the normal single-signature cases in
// matchesTargetFilter, with that filter's badge.
bool matchesMetaComposite(NimBLEAdvertisedDevice* dev, const char*& outLabel) {
    outLabel = nullptr;
    if (!dev) return false;

    // Condition A: mfr CID 0x0D53 (Luxottica) + svc UUID 0xFD5F (Meta) in
    // the same advert. Both signals required.
    bool haveLuxottica = false;
    if (dev->haveManufacturerData()) {
        std::string mfr = dev->getManufacturerData();
        if (mfr.length() >= 2 && (uint8_t)mfr[0] == 0x53 && (uint8_t)mfr[1] == 0x0D) {
            haveLuxottica = true;
        }
    }
    if (haveLuxottica) {
        for (int i = 0; i < dev->getServiceUUIDCount(); i++) {
            NimBLEUUID uuid = dev->getServiceUUID(i);
            String s = uuid.toString().c_str();
            s.toLowerCase();
            bool short16 = (s.length() == 4 && s.equals("fd5f"));
            bool long128 = (s.length() >= 8 && s.substring(4, 8).equals("fd5f"));
            if (short16 || long128) {
                outLabel = "META-RAYBAN (mfr+svc)";
                return true;
            }
        }
    }

    // Condition B: name substring. Complete local name only — NimBLE's
    // haveName() covers both complete + shortened, that's fine here.
    if (dev->haveName()) {
        String name = dev->getName().c_str();
        if (nameContains(name, "Ray-Ban") ||
            nameContains(name, "Wayfarer") ||
            nameContains(name, "Oakley Meta")) {
            outLabel = "META-RAYBAN (name)";
            return true;
        }
    }

    return false;
}

// ================================
// Detection Presets
// ================================
//
// One-click "add every known signature for a device family" bundles.
// Signatures triple-sourced: Bluetooth SIG assigned-numbers registry
// (company IDs + 16-bit service UUIDs), IEEE OUI registry (MAC prefixes),
// and cross-referenced against the community friendorfoe project
// (lnxgod/friendorfoe/esp32/scanner/main/detection/ble_fingerprint.c)
// which ships the same discrimination logic on ESP32-S3 hardware.

struct PresetEntry {
    FilterType type;
    const char* identifier;
    const char* description;
};

// Meta / Ray-Ban smart glasses (Wayfarer, Headliner, Skyler, Oakley Meta).
// One synthetic composite signature rather than separate CID/UUID entries:
// the glasses use RPA (rotating random MAC per BT spec), so OUI matching is
// pure noise, and CID-alone or UUID-alone filters were false-positive
// magnets (0xFD5F is advertised by phones running Meta apps). The composite
// requires mfr CID 0x0D53 (Luxottica) AND svc UUID 0xFD5F (Meta) in the
// same advert, or a name-substring hit — see matchesMetaComposite().
static const PresetEntry PRESET_META[] = {
    { FT_META_COMPOSITE, "0x0D53+0xFD5F", "Composite: Luxottica CID + Meta svc UUID, or Ray-Ban/Wayfarer/Oakley Meta name" },
};
static const size_t PRESET_META_COUNT = sizeof(PRESET_META) / sizeof(PRESET_META[0]);

// Axon body cameras (Body 3/4, Fleet dash, Taser 7/10).
// Uses all five signal types: dedicated IEEE OUI 00:25:DF ("Axon
// Enterprise, Inc."), Bluetooth SIG company ID 0x034D ("TASER
// International, Inc." — Axon's earlier registered name), service
// UUID 0xFC81 ("Axon Enterprise, Inc."), WiFi probe requests, and
// WiFi beacons. All uniquely attributable.
static const PresetEntry PRESET_AXON[] = {
    { FT_MAC_PREFIX,      "0025DF", "Axon Enterprise OUI (IEEE)" },
    { FT_COMPANY_ID,      "034D",   "TASER International CID (Axon body cams)" },
    { FT_SERVICE_UUID_16, "FC81",   "Axon Enterprise service UUID" },
    { FT_WIFI_PROBE,      "0025DF", "Axon body cam (WiFi probe)" },
    { FT_WIFI_BEACON,     "0025DF", "Axon AP beacon (WiFi)" },
};
static const size_t PRESET_AXON_COUNT = sizeof(PRESET_AXON) / sizeof(PRESET_AXON[0]);

// I-PRO body cameras — Bluetooth LE + WiFi domain. Panasonic subsidiary with 
// dedicated OUI.
// Preset is for both BLE advertisement MACs and WiFi probe requests.
static const PresetEntry PRESET_IPRO[] = {
    { FT_MAC_PREFIX,   "D42DC5", "I-PRO body cam (BLE OUI)" },
    { FT_WIFI_PROBE,   "D42DC5", "I-PRO body cam (WiFi probe)" },
};
static const size_t PRESET_IPRO_COUNT = sizeof(PRESET_IPRO) / sizeof(PRESET_IPRO[0]);

// Returns count added. Skips entries whose (type, identifier) already
// exists so repeated clicks don't duplicate rows.
int applyPreset(const PresetEntry* preset, size_t count, const char* labelPrefix) {
    int added = 0;
    for (size_t i = 0; i < count; i++) {
        const PresetEntry& p = preset[i];
        bool exists = false;
        for (const TargetFilter& f : targetFilters) {
            if (f.type == p.type && f.identifier.equalsIgnoreCase(p.identifier)) {
                exists = true;
                break;
            }
        }
        if (exists) continue;

        TargetFilter f;
        f.type        = p.type;
        f.identifier  = p.identifier;
        f.isFullMAC   = (p.type == FT_FULL_MAC);
        f.description = String(labelPrefix) + ": " + p.description;
        targetFilters.push_back(f);
        added++;
    }
    if (added > 0) {
        saveConfiguration();
        rebuildWifiOuiTable();
    }
    return added;
}

// Remove the signatures a preset installed.
int removePreset(const PresetEntry* preset, size_t count) {
    int removed = 0;
    for (size_t i = 0; i < count; i++) {
        const PresetEntry& p = preset[i];
        for (size_t j = 0; j < targetFilters.size(); ) {
            if (targetFilters[j].type == p.type &&
                targetFilters[j].identifier.equalsIgnoreCase(p.identifier)) {
                targetFilters.erase(targetFilters.begin() + j);
                removed++;
            } else {
                j++;
            }
        }
    }
    if (removed > 0) saveConfiguration();
    return removed;
}

// True if any of the preset's signatures are currently installed.
bool presetInstalled(const PresetEntry* preset, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const PresetEntry& p = preset[i];
        for (const TargetFilter& f : targetFilters) {
            if (f.type == p.type && f.identifier.equalsIgnoreCase(p.identifier)) return true;
        }
    }
    return false;
}

// ============================================================================
// Boot Button -> Config Mode (runs every loop, works while scanning)
// ============================================================================
// Mirrors the runtime handler in oui-spy-unified-blue. Holding BOOT (GPIO0)
// for 1.5s from any state clears the burn-in lock and reboots into config
// mode. Without this, burn-in is a one-way door: the AP never comes back, so
// there is no way to reach the dashboard and no way to undo it short of
// erasing flash.
#define BOOT_BUTTON_PIN     0
#define BOOT_HOLD_TIME      1500

static unsigned long bootBtnStart    = 0;
static bool bootBtnActive            = false;

static void checkBootButtonLoop() {
    unsigned long now = millis();

    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
        if (!bootBtnActive) {
            bootBtnActive = true;
            bootBtnStart = now;
        }
        // Hold for 1.5 s -> clear burn-in lock, reboot to config mode
        else if (now - bootBtnStart >= BOOT_HOLD_TIME) {
            Serial.println("\n*** BOOT HELD -> clearing lock, returning to config mode ***");
            Serial.flush();
            for (int i = 0; i < 3; i++) {
                ledcSetup(0, 3000, 8);
                ledcAttachPin(BUZZER_PIN, 0);
                ledcWrite(0, 100); delay(80);
                ledcWrite(0, 0);   delay(60);
            }
            preferences.begin("ouispy", false);
            preferences.remove("configLocked");    // no-op if never locked
            preferences.end();
            delay(200);
            ESP.restart();
        }
    } else {
        // Button released — hold time already handled above while pressed
        bootBtnActive = false;
    }
}

// ================================
// WiFi Promiscuous Mode Detection
// ================================

// Promiscuous callback — runs in driver context. No String, no Serial, no delay.
// Fast path: frame-control check + 3-byte OUI pre-check against wifiOuiTable.
// Slow path: full filter match + dedup + enqueue.
static void handleWifiMatch(const uint8_t mac[6], uint8_t subtype, int8_t rssi) {
    // Format MAC as "aa:bb:cc:dd:ee:ff" (lowercase, matches NimBLE toString()
    // output) so a device seen by both radios dedups under one entry
    char macStr[18];
    int pos = 0;
    for (int i = 0; i < 6; i++) {
        if (i > 0) macStr[pos++] = ':';
        char hi = (mac[i] >> 4) & 0x0F;
        char lo = mac[i] & 0x0F;
        macStr[pos++] = hi > 9 ? 'a' + hi - 10 : '0' + hi;
        macStr[pos++] = lo > 9 ? 'a' + lo - 10 : '0' + lo;
    }
    macStr[pos] = '\0';

    char desc[64], ident[18];
    FilterType outType;
    if (!matchesWifiFilter(mac, subtype, desc, ident, outType)) return;

    // De-dup check: use the devices vector with (MAC, type) as key
    for (auto& dev : devices) {
        if (strcmp(dev.macAddress.c_str(), macStr) == 0 && dev.matchedType == outType) {
            unsigned long now = millis();
            if (dev.inCooldown && now < dev.cooldownUntil) return;  // still cooling
            if (dev.inCooldown) {
                dev.inCooldown = false;
                // Re-alert after cooldown — use single producer with overflow guard
                enqueueDetection(macStr, ident, desc, rssi, outType, "RE", true);
                dev.lastSeen = now;
                return;
            }
            break;
        }
    }

    // First detection of this (MAC, type) — use single producer with overflow guard
    enqueueDetection(macStr, ident, desc, rssi, outType, "NEW", true);
}

// promiscuous API: the callback is `void (*)(void* buf, wifi_promiscuous_pkt_type_t type)`.
// buf is a `wifi_promiscuous_pkt_t*` with an `rx_ctrl` control header followed by the
// actual 802.11 frame in `payload`. Do NOT treat buf as raw frame bytes.
void promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;

    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    if (pkt->rx_ctrl.sig_len < sizeof(wifi_ieee80211_mac_hdr_t)) return;

    wifi_ieee80211_mac_hdr_t* hdr = (wifi_ieee80211_mac_hdr_t*)pkt->payload;
    int8_t rssi = pkt->rx_ctrl.rssi;

    uint8_t fc0 = hdr->frame_ctrl & 0xFF;
    uint8_t ftype = (fc0 >> 2) & 0x03;
    uint8_t subtype = (fc0 >> 4) & 0x0F;

    // 802.11 mgmt subtypes: 0x00=Assoc Req, 0x04=Probe Req, 0x08=Beacon
    if (ftype != 0) return;  // management only
    if (subtype != 0x04 && subtype != 0x08) return;  // probe-req(4) / beacon(8) only

    // Source MAC is always in addr2 for probe requests and beacons.
    // 3-byte OUI pre-check against target table.
    uint8_t oui[3] = { hdr->addr2[0], hdr->addr2[1], hdr->addr2[2] };
    if (!wifiOuiInTargets(oui)) return;

    // Rare path: full match, de-dup, enqueue
    handleWifiMatch(hdr->addr2, subtype, rssi);
}

// Start WiFi promiscuous sweep. Enables MGMT-only capture.
static void startWifiSweep() {
    if (isSerialConnected()) Serial.println("[SWEEP] Starting WiFi sweep...");
    esp_wifi_set_promiscuous(false);
    delay(100);
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous(true);
}

// Stop WiFi promiscuous sweep. 50 ms drain delay before disabling.
static void stopWifiSweep() {
    delay(50);
    esp_wifi_set_promiscuous(false);
    if (isSerialConnected()) Serial.println("[SWEEP] WiFi sweep done");
}

// Start BLE scan for 2 seconds (matches WiFi sweep duration).
static void startBleScan() {
    if (isSerialConnected()) Serial.println("[SWEEP] Starting BLE scan...");
    pBLEScan->start(2, nullptr, false);
}

// Stop BLE scan.
static void stopBleScan() {
    if (pBLEScan) pBLEScan->stop();
    if (isSerialConnected()) Serial.println("[SWEEP] BLE scan done");
}

// Forward declaration for callback class
class MyAdvertisedDeviceCallbacks;

// BLE Advertised Device Callback Class
class MyAdvertisedDeviceCallbacks: public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        if (currentMode != SCANNING_MODE) return;

        String mac = advertisedDevice->getAddress().toString().c_str();
        int rssi = advertisedDevice->getRSSI();
        unsigned long currentMillis = millis();

        String matchedDescription;
        String matchedIdent;
        FilterType matchedTypeOut = FT_MAC_PREFIX;
        bool matchFound = matchesTargetFilter(advertisedDevice, mac,
                                              matchedDescription, matchedIdent,
                                              matchedTypeOut);

        // Meta / Ray-Ban composite detection runs inside matchesTargetFilter
        // when (and only when) the META preset is installed — no filter, no
        // trigger.

        if (matchFound) {
            bool known = false;
            for (auto& dev : devices) {
                if (dev.macAddress == mac) {
                    known = true;

                    if (dev.inCooldown && currentMillis < dev.cooldownUntil) {
                        return;
                    }

                    if (dev.inCooldown && currentMillis >= dev.cooldownUntil) {
                        dev.inCooldown = false;
                    }

                    unsigned long timeSinceLastSeen = currentMillis - dev.lastSeen;

                    if (timeSinceLastSeen >= 30000) {
                        enqueueDetection(mac, matchedIdent, matchedDescription, rssi, matchedTypeOut, "RE-30s", false);
                        dev.inCooldown = true;
                        dev.cooldownUntil = currentMillis + 10000;
                    } else if (timeSinceLastSeen >= 3000) {
                        enqueueDetection(mac, matchedIdent, matchedDescription, rssi, matchedTypeOut, "RE-3s", false);
                        dev.inCooldown = true;
                        dev.cooldownUntil = currentMillis + 3000;
                    }

                    dev.lastSeen = currentMillis;
                    break;
                }
            }

            if (!known) {
                DeviceInfo newDev;
                newDev.macAddress = mac;
                newDev.rssi = rssi;
                newDev.firstSeen = currentMillis;
                newDev.lastSeen = currentMillis;
                newDev.inCooldown = false;
                newDev.cooldownUntil = 0;
                newDev.matchedFilter = matchedDescription.c_str();
                newDev.filterDescription = matchedDescription;
                newDev.matchedIdentifier = matchedIdent;
                newDev.matchedType = matchedTypeOut;
                devices.push_back(newDev);

                // LRU-drop oldest so the session cap holds even with a
                // firehose of unique MACs.
                while (devices.size() > 200) {
                    devices.erase(devices.begin());
                }

                // Store data for main loop to process (beep + flash happen in
                // loop() drain — calling them here blocks the NimBLE host task)
                enqueueDetection(mac, matchedIdent, matchedDescription, rssi, matchedTypeOut, "NEW", false);

                auto& dev = devices.back();
                dev.inCooldown = true;
                dev.cooldownUntil = currentMillis + 3000;
            }
        }
    }
};

// ================================
// Scanning State & Phase Transition Functions
// ================================
static bool g_scanInitialized = false;
enum RadioPhase { PHASE_IDLE, PHASE_WIFI_SWEEP, PHASE_BLE_SCAN };
static RadioPhase g_radioPhase = PHASE_IDLE;
static uint8_t g_wifiChannel = 11;
static unsigned long g_phaseStartTime = 0;
static unsigned long g_lastHopTime = 0;
static const unsigned long WIFI_SWEEP_MS = 2200;
static const unsigned long BLE_SCAN_MS   = 2200;
static const unsigned long WIFI_HOP_MS   = 200;

// Dual-domain time-slice architecture:
// BLE and WiFi share the ESP32-S3's single 2.4 GHz antenna; simultaneous promiscuous
// RX + BLE scan is rated "C1" (unstable) per ESP-IDF. We time-slice to avoid: WiFi sweep
// (hop ch 11→1 at 200ms) and BLE scan (single 2s active scan) alternate, each ~2.2s.
// STA association is mutually exclusive with promiscuous RX on this hardware, so STA
// must be disconnected first. Alerting (beeps, LED) runs in the main loop's ring-buffer
// drain, not in NimBLE/WiFi callbacks. The promiscuous callback runs in driver context and must reject frames
// cheaply, so a 3-byte OUI table (wifiOuiTable) is rebuilt when WiFi filters change.
// One-time scanning init (idempotent). Must run before any scan_to_* call.
// Sequence: stop config services -> init BLE -> set up WiFi -> idle.
static bool scan_init() {
    if (g_scanInitialized) return true;

    // (a) Stop config services
    dnsServer.stop();
    server.end();
    WiFi.softAPdisconnect(true);

    // (c) Compute filter presence
    bool hasWifiFilters = false;
    bool hasBleFilters = false;
    for (const TargetFilter& f : targetFilters) {
        if (isWifiDomain(f.type)) hasWifiFilters = true;
        else hasBleFilters = true;
    }

    // (b) BLE init — MUST happen before WiFi mode is set to STA/promiscuous
    if (hasBleFilters) {
        NimBLEDevice::init("");
        delay(1000);
        pBLEScan = NimBLEDevice::getScan();
        if (pBLEScan != nullptr) {
            pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
            pBLEScan->setActiveScan(true);
            pBLEScan->setInterval(300);
            pBLEScan->setWindow(200);
        }
    }

    // (d) WiFi setup — always STA if WiFi filters exist, connect only if MQTT enabled
    bool needsWifi = hasWifiFilters;
    if (needsWifi) {
        WiFi.mode(WIFI_STA);
        if (mqttCfg.enabled && mqttCfg.sta_ssid[0] && mqttCfg.broker[0]) {
            WiFi.begin(mqttCfg.sta_ssid, mqttCfg.sta_pass);
            if (isSerialConnected()) Serial.printf("WiFi STA connecting to %s\n", mqttCfg.sta_ssid);
            unsigned long ws = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - ws < 10000) delay(250);
            if (WiFi.status() == WL_CONNECTED) {
                if (isSerialConnected()) Serial.println("WiFi connected: " + WiFi.localIP().toString());
                mqtt_connect();
            } else {
                if (isSerialConnected()) Serial.println("WiFi STA failed, continuing offline");
            }
        }
    } else {
        WiFi.mode(WIFI_OFF);
    }

    // (e) Start idle
    g_radioPhase = PHASE_IDLE;
    g_scanInitialized = true;
    if (isSerialConnected()) Serial.println("[SWEEP] Scanning initialized");
    return true;
}

// Transition to WiFi promiscuous sweep phase (idempotent).
// STA must be disconnected before promiscuous can start.
static void scan_to_wifi_sweep() {
    if (g_radioPhase == PHASE_WIFI_SWEEP) return;

    // Stop BLE scan if currently running
    if (g_radioPhase == PHASE_BLE_SCAN && pBLEScan) {
        pBLEScan->stop();
        delay(50);
    }

    // Ensure STA is disconnected before promiscuous
    if (WiFi.status() != WL_DISCONNECTED) {
        WiFi.disconnect();
        delay(100);
    }

    // Enable promiscuous mode (MGMT frames only)
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
    esp_wifi_set_promiscuous(true);

    g_radioPhase = PHASE_WIFI_SWEEP;
    g_wifiChannel = 11;
    esp_wifi_set_channel(g_wifiChannel, WIFI_SECOND_CHAN_NONE);
}

// Transition to BLE scan phase (idempotent).
static void scan_to_ble_scan() {
    if (g_radioPhase == PHASE_BLE_SCAN) return;

    // Stop WiFi promiscuous if running
    if (g_radioPhase == PHASE_WIFI_SWEEP) {
        esp_wifi_set_promiscuous(false);
        delay(50);
    }

    // Start BLE scan (2 seconds)
    if (pBLEScan) {
        pBLEScan->start(2, nullptr, false);
    }

    g_radioPhase = PHASE_BLE_SCAN;
}

// Hop to the next WiFi channel while sweeping (11 -> 10 -> ... -> 1 -> 11).
static void scan_wifi_hop_channel() {
    if (g_radioPhase != PHASE_WIFI_SWEEP) return;
    g_wifiChannel = (g_wifiChannel > 1) ? g_wifiChannel - 1 : 11;
    esp_wifi_set_channel(g_wifiChannel, WIFI_SECOND_CHAN_NONE);
}

// Clean up scanning state and return radio to off (idempotent).
// Does NOT change currentMode or start config mode.
static void scan_exit() {
    if (!g_scanInitialized) return;

    // Stop whatever radio activity is in progress
    if (g_radioPhase == PHASE_WIFI_SWEEP) {
        esp_wifi_set_promiscuous(false);
    } else if (g_radioPhase == PHASE_BLE_SCAN && pBLEScan) {
        pBLEScan->stop();
    }

    WiFi.mode(WIFI_OFF);
    g_scanInitialized = false;
    g_radioPhase = PHASE_IDLE;
    if (isSerialConnected()) Serial.println("[SWEEP] Scanning exited");
}

// ================================
// Device Alias Functions
// ================================
void saveDeviceAliases() {
    preferences.begin("ouispy", false);
    preferences.putInt("aliasCount", deviceAliases.size());
    
    for (int i = 0; i < deviceAliases.size(); i++) {
        String keyMac = "alias_mac_" + String(i);
        String keyName = "alias_name_" + String(i);
        
        preferences.putString(keyMac.c_str(), deviceAliases[i].macAddress);
        preferences.putString(keyName.c_str(), deviceAliases[i].alias);
    }
    
    preferences.end();
    
    if (isSerialConnected()) {
        Serial.println("Device aliases saved to NVS (" + String(deviceAliases.size()) + " aliases)");
    }
}

void loadDeviceAliases() {
    preferences.begin("ouispy", true);
    int aliasCount = preferences.getInt("aliasCount", 0);
    
    deviceAliases.clear();
    
    for (int i = 0; i < aliasCount; i++) {
        String keyMac = "alias_mac_" + String(i);
        String keyName = "alias_name_" + String(i);
        
        DeviceAlias alias;
        alias.macAddress = preferences.getString(keyMac.c_str(), "");
        alias.alias = preferences.getString(keyName.c_str(), "");
        
        if (alias.macAddress.length() > 0 && alias.alias.length() > 0) {
            deviceAliases.push_back(alias);
        }
    }
    
    preferences.end();
    
    if (isSerialConnected()) {
        Serial.println("Device aliases loaded from NVS (" + String(deviceAliases.size()) + " aliases)");
    }
}

String getDeviceAlias(const String& macAddress) {
    String normalizedMAC = macAddress;
    normalizeMACAddress(normalizedMAC);
    
    for (const DeviceAlias& alias : deviceAliases) {
        String normalizedAliasMAC = alias.macAddress;
        normalizeMACAddress(normalizedAliasMAC);
        
        if (normalizedAliasMAC.equals(normalizedMAC)) {
            return alias.alias;
        }
    }
    
    return ""; // No alias found
}

void setDeviceAlias(const String& macAddress, const String& alias) {
    String normalizedMAC = macAddress;
    normalizeMACAddress(normalizedMAC);
    
    // Check if alias already exists, update it
    for (auto& deviceAlias : deviceAliases) {
        String normalizedAliasMAC = deviceAlias.macAddress;
        normalizeMACAddress(normalizedAliasMAC);
        
        if (normalizedAliasMAC.equals(normalizedMAC)) {
            if (alias.length() > 0) {
                deviceAlias.alias = alias;
            } else {
                // Remove alias if empty - find and remove the entry
                for (size_t i = 0; i < deviceAliases.size(); i++) {
                    String mac = deviceAliases[i].macAddress;
                    normalizeMACAddress(mac);
                    if (mac.equals(normalizedMAC)) {
                        deviceAliases.erase(deviceAliases.begin() + i);
                        break;
                    }
                }
            }
            return;
        }
    }
    
    // Add new alias if not empty
    if (alias.length() > 0) {
        DeviceAlias newAlias;
        newAlias.macAddress = normalizedMAC;
        newAlias.alias = alias;
        deviceAliases.push_back(newAlias);
    }
}

// ================================
// Persistent Device Storage Functions
// ================================
void saveDetectedDevices() {
    preferences.begin("ouispy", false);
    
    // Limit to 100 most recent devices to avoid NVS overflow
    int deviceCount = min((int)devices.size(), 100);
    preferences.putInt("deviceCount", deviceCount);
    
    for (int i = 0; i < deviceCount; i++) {
        String keyMac = "dev_mac_" + String(i);
        String keyRssi = "dev_rssi_" + String(i);
        String keyTime = "dev_time_" + String(i);
        String keyFilt = "dev_filt_" + String(i);
        
        preferences.putString(keyMac.c_str(), devices[i].macAddress);
        preferences.putInt(keyRssi.c_str(), devices[i].rssi);
        preferences.putULong(keyTime.c_str(), devices[i].lastSeen);
        preferences.putString(keyFilt.c_str(), devices[i].filterDescription);
    }
    
    preferences.end();
}

void loadDetectedDevices() {
    preferences.begin("ouispy", true);
    int deviceCount = preferences.getInt("deviceCount", 0);
    
    devices.clear();
    
    for (int i = 0; i < deviceCount; i++) {
        String keyMac = "dev_mac_" + String(i);
        String keyRssi = "dev_rssi_" + String(i);
        String keyTime = "dev_time_" + String(i);
        String keyFilt = "dev_filt_" + String(i);
        
        DeviceInfo device;
        device.macAddress = preferences.getString(keyMac.c_str(), "");
        device.rssi = preferences.getInt(keyRssi.c_str(), 0);
        device.lastSeen = preferences.getULong(keyTime.c_str(), 0);
        device.filterDescription = preferences.getString(keyFilt.c_str(), "");
        device.firstSeen = device.lastSeen;
        device.inCooldown = false;
        device.cooldownUntil = 0;
        device.matchedFilter = nullptr;
        
        if (device.macAddress.length() > 0) {
            devices.push_back(device);
        }
    }
    
    preferences.end();
    
    if (isSerialConnected()) {
        Serial.println("Detected devices loaded from NVS (" + String(deviceCount) + " devices)");
    }
}

void clearDetectedDevices() {
    devices.clear();

    preferences.begin("ouispy", false);
    preferences.putInt("deviceCount", 0);
    preferences.end();

    if (isSerialConnected()) {
        Serial.println("All detected devices cleared from memory and NVS");
    }
}

// ================================
// Web Server HTML
// ================================
const char* getASCIIArt() {
    return R"(
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                           @@@@@@@@                                                         @@@@@@@@                                        
                                                                                                                                                                                                       @@@ @@@@@@@@@@                                                    @@@@@@@@@@ @@@@                                    
                                              @@@@@                                                           @@@@@                                                                               @@@@ @ @ @@@@@@@@@@@@@                                               @@@@@@@@@@@@ @@@@@@@@                                
                                         @@@@ @@@@@@@@                                                     @@@@@@@@@@@@@                                                                     @@@@ @@@@@@@@@@@@@@@@@@@@@@@@                                          @@@@@@@@@@@@@@@@@@@ @@@@@@@@@                           
                                     @@@@@@@@ @@@@@@@@@@                                                 @@@@@@@@@@@@ @@ @@@@                                                            @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                                    @@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@                       
                                @@@@@@@@@@@@@@@@@@@@@@@@@@@                                           @@@@@@@@@@@@@@@@@@@@@@@@@@@                                                        @@@@@@ @@@@@@@@@          @@@@@@@@@@@@                                @@@@@@@@@@@@@          @@@@@@@@@@@@@@@                       
                           @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                                      @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                                                   @@@@@@@@@ @@@               @@@@@@@@@@@@@                          @@@@@@@@@@@@@               @@@@@@@@@@@@@                       
                          @@@ @@@@@@@@@@@@@       @@@@@@@@@@@@@@                                 @@@@@@@@@@@@@@      @@@@@@@@@@@@@@@@@@                                                  @@ @@@@@@@@@                  @@@@@@@@@@@@@@                     @@@@@@@@ @@@@                   @@@@@  @@@@                       
                          @@@@ @@@@@@@@@              @@@@@@@@@@@@                            @@@@@@@@@@@@@              @@@@@@@@@ @@ @                                                  @@@@   @@@@                   @@@@@@@@@@@ @@                     @ @@@@@@@@@@@                    @@@@  @ @@                       
                          @@@@@@@ @@@                   @@@@@@@@@@@@@                       @@@@@@@@@@@@@                  @@@@ @@@@@@@                                                   @@@  @@@@                     @@ @@@@@@@@@@                     @@@@@@@@@ @@@                     @@@  @@ @                       
                          @@@@@  @ @@                   @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@                   @@@@  @@@@                                                    @@@  @@@@                     @@@  @@ @                              @ @@@@@                      @@@@ @@@@                       
                           @@@   @@@                     @@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                    @@@@   @@@                                                    @@@@ @@@@                    @@@@  @@@@                              @@@@@@@@                    @@@@@@@@@@                       
                           @@@@ @@@@                     @@ @@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@   @@@                     @@@  @@@@                                                    @@@@ @@@@@                   @@@   @ @                                 @ @@@@@                  @@@@@@@@@@                        
                           @@@@ @@@@                     @@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  @@@                     @@@@ @@@@                                                    @@@@@@@ @@@                @@@@@   @ @                                 @ @ @@@@                @@@@@@@@@ @                        
                           @@@@ @@@@@                   @@@ @ @                                @@@@  @@@@                   @@@@@ @@@@                                                     @@@@@@@@@@@@             @@@@@    @@@@                               @@@@  @@@@@            @@@@@@@  @  @                        
                           @@@@ @@ @@@                 @@@@ @ @                                 @ @   @@@@                 @@@ @@@@@@                                                      @@@ @@@ @@@@@@@@     @@@@@@@@     @@@@                               @@@@   @@@@@@@@    @@@@@@@@ @@ @@@@@                        
                            @@@@@@@@@@@@             @@@@@  @@@                                @@@@   @@@@@              @@@@@@@@@@@@                                                      @@@@@@@   @@@@@@@@@@@@@@@@@        @@@                               @@@      @@@@@@@@@@@@@@@@@  @@ @@@@@                        
                            @@@@ @@ @@@@@@         @@@@@@   @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@     @@@@@@        @@@@@@@ @@ @@ @                                                      @@@@@@@       @@@@@@@@@  @@@@@@@            @@@@@@@@          @@@     @@@@  @    @@@@@@@@@@      @@ @@@@@                        
                            @@ @@@@@ @@@@@@@@@@@@@@@@@@     @@@@@                             @@@@       @@@@@@@@@@@@@@@@@@   @@@@ @@                                                      @@@@@@@       @@@  @@   @@ @@@@@           @@@@  @ @          @ @     @@@@@@ @     @ @           @@ @ @@                         
                            @@ @ @@@  @@ @@@@@@@@@@@@@@@@@@   @@@@@@@ @@@@@@@@ @@@@@@@@@@@@@@@@@@@        @@ @@@@@@@@@@@@@@@ @@@@@@@@                                                      @@ @@@@      @@@@@@@@@@  @@@@@@ @@@        @@@@@@@@@          @@@@@   @@@@   @@@   @@@@@@@@      @@ @@@@                         
                            @@@@ @@@  @@@@     @@@@  @@@@@@     @ @@@@@   @@@@@@@@@        @@@@@@@@@@@@   @ @ @@@@@@@@@  @@@@@@@@@@@@                                                       @@@@@@@  @@@ @@  @@@@@@@@@    @@@@         @@@@@@@   @@@@@   @@@@@@ @@@  @ @@@@@@@@@@@@@@@      @@@@@ @                         
                            @@@@@@@@  @@@@  @@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  @@@@@@@@@@ @@@@@@@@ @ @@@@@@@@@@@@@@@ @@@@                                                        @@@@@@@  @ @ @@  @@@@@@@@@@   @@@@          @@@@@@   @@@@@   @@@@@@ @ @   @@@@@@@@ @@  @@@      @@@@@@@                         
                             @@@@ @@  @ @ @@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@ @@@@ @@@@@@@@@@@ @@@@@@@@@@@@   @  @@@@@@@ @@@@@@ @@@@                                                        @ @@ @@  @@@@ @  @@@@@@@@@@@@@@@            @@@@@@   @@@@@   @@@@@@ @@@@  @@@  @@@@@@@@@@@      @@@@@@@                         
                             @  @ @@  @@@@@@@@@@@@@@@ @@@@@@@ @@@@@@@@@@@@ @@@  @@@@@@@@@@@@@@@@@ @  @@ @@@@  @  @@@@@ @@@   @@ @@ @                                                        @ @@@@@  @@@ @@  @@@@@ @@ @ @@ @@           @@@@@@   @@@@@   @@@@@@@@@@           @@@@@@       @@@@  @                          
                             @@ @ @@  @@@@@@@@@@@@@@@ @@@@@@@@@@@   @@@@@@ @@@@ @@ @@@@@@@@@@@@@@ @@@@@@@@@@  @@@@@@@@@@     @@@@@ @                                                        @@ @@@@  @@@@    @@@@@@   @@@@@@@           @  @@@   @@@@@   @@@@@@@@@@           @@@@ @       @@@@@@@                          
                             @@@@@@@  @@@@@@    @@@ @ @@ @@@@@@@@@   @@@@@@@@@@ @@@@@@@@@@@ @@@@@ @ @@ @@@@@  @@@@@ @@       @@@@@@                                                          @@@@@@  @@@@    @@@@@@       @@@           @@@@@@   @@@@@   @@@@@@@@@@           @@@@@@       @@@@@@@                          
                             @@ @@@@  @@@@@@@@@@@@@@@ @@@@@@@    @@@@@@ @@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@    @@@@@@@@       @@@@@@                                                          @@@@@@  @@@      @ @ @@@@@@  @@@@ @        @@@@@@   @@@@@   @@@  @@@@@   @@@@@@  @@@@         @@@@@@                           
                              @  @@@  @@@@@@@@ @@@@@@ @@@@@@@    @@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@ @ @@@@@@    @@@@@@@@@      @@@@@@                                                          @@@@@@   @@@    @@@@ @ @@@@@@@@@@ @        @@@@@@   @@ @@      @ @@@@@@@@@@@@@@  @@@@ @       @@@@@@                           
                              @@ @@@   @@@@@@@@@@@@   @@@@@@@     @@@@@@ @@@@@@@@@@@@@@    @@@@@@@@@@@@@@@    @@@@@@@@@      @@@@@@                                                           @@@@@   @ @    @@@@ @@@@@@@@@@@ @@        @@@@@@   @@@@@      @@@@@@@ @ @@@@@@  @@@@         @@@  @                           
                              @@@@@@      @@@@ @@@       @@@@             @@@ @@@@@      @@@@   @   @@@       @@@  @@@@      @@@@@                                                            @@@@@   @@@     @@@     @@@@@@@           @@@                      @@@@@@@@@    @@@@         @@@@@@                           
                              @@@@@@@        @@       @@@@@   @@@@@@      @@@@@@@@@@@@@@@@   @    @@@@@@@@@@@@              @@ @@@                                                            @@@ @                                                                                        @@@@@@                           
                              @@@@@@@      @@@@@      @ @@@@@ @@@@@@@@@   @@@@@ @@@@ @@@@ @@@@   @@@@@@@@  @@@              @@@@@@                                                            @@@@@@             @@@@@@@@@    @@@   @@@    @@@@@@@@@    @@@@@@@@     @@@@@@@@@             @@@@@                            
                               @  @@@      @@@@       @@@@@ @ @@@@@@@ @   @@@@@@@@@@@@@@@        @@@@@@@@@@@@@              @@@@ @                                                            @@@@@@             @@    @@@    @ @   @ @@@  @@@    @@    @@ @@@@@     @@@    @@@            @ @@@                            
                               @@@@@@      @@@@       @@@@@@@ @@@@@@@@ @@@@@@@@      @@@@      @@@@@@@     @@ @@@@@         @@@@@@                                                            @@@@@@             @@@@@@@@@@@@ @@@   @@@@@  @@@@@@@ @@@@  @@@@@@@@@@@  @@@@@@ @@@@          @ @@@                            
                               @@@@@@     @@@@@      @@@@@@@@ @@@@@@  @@ @@@@@@      @@@@      @@@@@@@@      @@@@@@         @@@@@                                                              @@@@@           @@@@@   @@ @@@ @@@@  @@@@@@@@@@   @@@@@@@@@@   @@@@@@@@@@   @@@@@@         @@@@ @                            
                                 @@@@     @@@@@      @@@@@@@@ @@@@@@  @@@@@@@@@      @@@@      @@@@@@@@@@@@@@@@@@@@         @@@@@                                                              @@@ @           @@ @@@  @@@@@@ @@@@@ @@@ @@@@@@   @@@@@@@@@@   @@ @@@@@@@   @@@@@@         @@@@@@                            
                                @@@@@     @@@@@@@@@@@@@@@@@@@ @@@@@@     @@@@@@     @@@@@@@     @@@@@@@@@@@@@@ @@@@         @ @ @                                                              @@@@@           @@@@@@ @  @@@@ @@@@  @@@@@@@@@@   @@@@@@@@@@   @@@@@@@@@@   @@@@@@         @@@@@@                            
                                @@@ @     @@ @  @      @@@   @@@  @@      @@@@@     @@   @@         @@@@@@@@@  @@           @@ @@                                                              @@@@@              @@@  @  @@@ @@@  @@@@@@@@@@@   @@@ @@@@@@   @@ @@@@@@@   @@ @@@         @@@ @                             
                                @   @        @@@@@@@@@@@@@    @@@@@@    @ @@@@@@@@  @@@@@@@         @@@@@@@@@@@@            @@@@@                                                              @@@@                       @ @@@@@  @ @@ @ @@@@    @@ @@@@@@    @@@@@@@@@                    @@@                             
                                @@@@@      @@@@@@@@@@@@@@@@@@@  @@@@   @@@   @@@@@@  @@@@   @@@@@       @@@@@               @@@@@                                                               @@@                       @@@@@@@  @@@@@@@@@@@     @@@@@@@@    @@@@ @@@@                    @ @                             
                                @@@@@      @@ @@@  @@@ @  @@ @  @@@@   @ @@@@@ @@@@  @@@@   @ @@@@@@   @@@@@@                @@@                                                                @@@               @@@        @@@@   @@@  @@@@@   @@@  @@@@@        @@@@@                   @@@@                             
                                 @@@       @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@ @@@@@@@   @ @@@@@@@@@@@@@ @@               @@@                                                                @ @              @@@@@@@@@@@ @@@@   @@@@ @@@@@@@@@@@@@ @@@@  @@@@  @@@@@                   @@@@                             
                                 @@@              @@@@@     @@@@@@@@@@@@@@@@@  @@@@@@@@@@   @@@@@@@@@@@@@@@@@@@@             @ @                                                                @ @              @@@@@@@@@ @  @ @   @@@     @@@@@@@@ @  @ @     @    @ @                   @ @                              
                                 @ @              @@@@@     @@@@@@@@@@@@@@@@@  @@@@@@@@@@   @ @@@@@@@@@@ @@@@@ @             @ @                                                                @@@              @@@@@@@@@@@  @@@   @@@@    @@@@@@@@@@  @@@  @@@@    @@@                   @ @                              
                                 @@@@             @@@@@     @@@@  @@@@@@@ @@@@@@@ @@ @@@@   @ @@@@@@@@@@@@@ @  @@@          @@@@                                                                 @@@                                                                                       @@@                              
                                 @@@@            @@@@@@@      @@@@@@    @@@ @@@@@ @@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@          @@@                                                                  @ @  @@@    @@@   @@@@   @@@   @@@@@@@@@@@@ @@@@@@@@@@         @@@   @@@@   @@@@@@@@@     @@@                              
                                  @@@  @@@@@@    @@@@ @@      @@@@@@    @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@ @@@@@@@  @@@                                                                  @@@  @ @    @@@@  @@@@   @@@@  @@@@@@@@  @@ @@ @ @ @@@@        @ @   @@@@   @@  @ @@@@   @@@@                              
                                  @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@    @@@@@@@@@@@ @ @@@@@@@ @@@@ @@@@@@ @@@@@@@@@@@@@@@@@@@@                                                                  @@@  @@@    @@@@  @@@@   @@@@  @@@@@@@@@@@@  @@@@@@@@@@        @@@   @@@@   @@@@@@@@@@   @@@@                              
                                  @ @@@@@  @@@@@@ @@@@@        @@ @@@@@@ @@@@@     @ @@@@@@@@@@ @@  @@@       @@@@@@@@  @@@@@ @                                                                  @ @ @@@     @@@@@@@@@@@  @@@@     @@@ @@   @@@@    @@@@        @@@   @@@@@@ @@    @@@@@@ @@@                               
                                  @@@@ @@@@@@ @@ @@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@   @@@ @                                                                  @@@@@@@@    @@@@@@@@@@@  @@@@@@   @@@@@@   @@@@@   @@@@@@    @@@@@   @@@@@@ @@@@ @@@ @ @ @@@                               
                                  @@@@@@@@@@@@ @@ @          @@@@@@@@@@@                 @@@@@@@ @@@          @ @@@@@@@@@@@@@@                                                                    @@@@@@@@   @@@@@@@@@@@  @@@@ @   @@ @@@   @@@@@   @@@@@@    @@ @@   @@ @@@ @@@@ @ @  @@ @@@                               
                                  @@@@@@@@@@@@@@@@@        @@@@@@@@@                          @@@@@@@@        @ @@@@ @@@@@@@@@                                                                    @@@@@@@    @@@@@@@@@    @@@@@@   @@@@@@   @@@@@   @@@@@@    @@@@@   @@@@@@ @@@@ @@@@   @@@                                
                                   @@@@@@@@@@@@@ @@      @@@@@@@                                @@@@@@@@      @@@ @@@@@@@@@@@@                                                                    @@@@@@@     @ @ @@@@@   @@@ @@   @@@@@    @@@@@    @@@@@    @@@@@   @@@@@@       @@@@@@@@@                                
                                   @@@@@@@@@@@@@@@@@   @@@  @@@@                                 @@@@@@@@@    @@@@@@@@@@@@@@@@                                                                    @ @@@@@     @ @ @@@@@   @ @@ @    @ @@    @@@@@   @@@@ @    @@@@@   @@@  @  @@@  @ @@ @@ @                                
                                   @@@ @@@@@@@@@@@@@ @@@@@@@@@                                      @@@@@@@@ @@@@ @@@  @@@@@@                                                                      @ @@@@    @@@@ @@@@@   @ @@@@   @@@@     @@@@@   @ @@@@    @@@@@   @@@@@@  @ @  @ @@@ @@@                                
                                   @@@@@@@@@@@@@ @@@@@@  @@                                         @@@@@ @@@@@@   @@@@@@@@@@                                                                      @@@@@  @@@@@@@ @@@@    @ @      @ @      @@@ @@@@@@@       @@@@@@@@@ @  @  @@@@@@ @  @@@@                                
                                    @@@@@@@@@@@@ @@@@@@@@@@                                          @@ @@@ @@@@   @@@@@@@@@@                                                                      @@@    @@@ @ @ @@@@    @ @      @ @      @@@@@@@@@ @           @@@@@ @ @@  @@@@ @ @  @ @                                 
                                    @@@  @@@@@   @@@@@ @@@                                            @@ @@@@@@@   @@ @@@ @@@                                                                      @@@@@@ @@@ @@@ @@@@    @@@      @@@       @@@@@@@@@@           @@@@@@@     @@@@ @@@@@@@@                                 
                                    @@@@@@@ @@   @@@@ @@@@                                             @@@@ @@@@   @@@@@@@@@@                                                                       @@@@@   @@@                              @@@@                               @@@   @@@@@                                 
                                    @@@  @@@@@@@@    @@@@    @@@@@@@                       @@@@@@@@@@@  @ @     @@@@@@@@ @@@                                                                        @ @@@  @@@@                              @@@@                               @@@  @@ @@@                                 
                                      @@ @@@@@ @@    @@@@  @@@@@@@@@@                      @@@@@@@@@@@  @@@@    @@ @@@@@ @@@                                                                        @@ @@  @@@@        @@@@                  @@@@                   @@@@        @ @  @@@@@@                                 
                                     @@@ @@@@@ @@    @ @   @@@@   @@@@                     @@       @@   @@@   @@@ @@@@@ @ @                                                                        @@@@@@ @@@         @@@@@@                @@@@@                @@@@@@        @ @  @@@ @                                  
                                     @@@  @@@@ @@    @ @   @@      @@@                     @@       @@   @ @   @@@ @@@@  @ @                                                                        @@@@@@ @@@         @@@@@@@             @@@@@@@@             @@@@ @@@        @@@@ @@@ @                                  
                                     @@@@ @@@@@@@    @ @   @@@@  @@@@@                     @@       @@   @@@   @@@@@@@@  @@@                                                                        @@@@@@ @@@          @@@@@@@@@@@@@@@@@ @@@@@ @@@@@@@@@@@@@@@@@@ @@@@         @@@@@@@@ @                                  
                                     @@ @@@@@@@@     @@@@ @@@@@@@@@@@                      @@@@@@@@@@@@ @@@@     @@@@@@@@@@                                                                          @ @@@ @@@           @@@@@@@@@   @@@@@@@@@@@@@@@@@@@@@  @@@@@@@@@            @@@@@@@@@                                  
                                      @@@ @@ @@@     @@@@@  @@@@@@@@                       @@@@@@@@@@@@@@ @      @@@@@   @@                                                                          @@@@@@@@@             @@@@@@@@@@@@ @@@ @@@@@@@@@@@ @@@@@@@@@@@              @@@@@@@@@                                  
                                      @@@@@@@@@@      @@@@@@                                   @@@   @@@@@@      @@@@@@@@ @                                                                          @@@@@@@@@              @@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@              @ @@@@@@@                                  
                                      @@@ @@@@@       @@@@@                                  @@@@@@@  @@@@@       @@@@ @@@@                                                                          @ @@@@@@@              @@ @@@@ @@@@@ @@@     @@ @@@@@@@@@@@@@               @ @@@@ @                                   
                                      @ @@@@@@@@@@    @@@@@                                  @@@ @@@ @@@@@@    @@@@@@@@@@@@                                                                          @@@ @@@@               @@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@ @@@@               @ @@@@ @                                   
                                      @@@@@@@@@ @@   @@@@@@                                  @@@@@@@ @@@@@@@   @@ @@@@@@@@@                                                                           @@@@@@@                @@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@               @@@@@@@@                                   
                                      @@@@@@@@@@@@   @@@@@ @@@                                 @@@   @@@ @ @   @@@@@@@@@@@                                                                            @@@@@@@                @@@@@@@@@   @@@@@@@@@@@@@  @@@@@@@@@@               @@@@ @@@                                   
                                       @ @@@@@@@@@   @@ @@@@@@                                        @ @@@ @@ @@@@@@@@@@@                                                                            @ @@@@@              @@@@@@@ @@       @@@@@@@@@@   @@@@@@ @@@              @@@@@@@@                                   
                                       @@@@@@@@@@@ @@ @@@@@@@@@@@@                             @@@    @@@@@@ @@@@@@@@ @@@@                                                                            @ @@@@@            @@@@@@@@@@@@       @@@@@@@@@    @@@@@@@@@@@@@            @@@@ @                                    
                                       @ @@@@@@@@@@@ @@ @ @@@ @@@@@@                        @@@@ @ @   @@@  @ @@@@@@@@@  @                                                                            @@@ @@@          @@@@ @@@@@@@@@       @@@@@@@@     @@@@@@@@@ @@@@@          @@@@@@                                    
                                       @@@@ @@@@ @@@@@@@@   @@@@@@@@ @@@@               @@@@@ @@@@     @@@  @@@@ @@@@@@@@@                                                                             @@@     @@@@@@@@@@@@@ @@@@@@@@    @@@@@@@@@@@@    @@@@@@@@@@@@@@@@@@@@@@      @@@                                    
                                       @@@@ @@@@ @ @ @@@@     @@@@@@@@@@@ @@@@@@@@@ @@@ @@@@@@@@       @@@@ @@@@ @@@  @@@                                                                              @ @     @@      @@@@@@@  @@@@@    @@  @@@@@@@@    @@@@@   @@@@@@@     @@      @ @                                    
                                        @ @ @@@@ @ @ @ @        @@@ @ @@@ @@@@@@ @@ @ @ @@@@  @@       @@@@ @@@@ @@@@@@@@                                                                              @@@     @@@@@@@@@@@@@@@@@@@@@@    @@@@@@@@@@@@    @@@@@@@@@@@@@@@@@@@@@@     @@@                                     
                                        @@@      @ @ @@@         @@@@@@@  @@@@@@@@@ @@@  @  @           @@@ @@@@     @@@@                                                                                               @@@@@ @@@@@@@        @@@@        @@@@@@@@ @@@@              @@@                                     
                                        @@@      @ @ @@@            @@ @@@                @@            @@@ @@@@     @@@                                                                               @ @                @@@@@ @@@@@       @@@@@@@      @@@@@@@@@@@                @ @                                     
                                        @@@      @ @ @ @             @@@ @                              @@@ @@@@     @@@                                                                               @ @                   @@@@@@@@       @@@@@@@      @@@@@@@@@                  @ @                                     
                                        @ @   @@@@ @ @@@               @@@                              @@@ @@@@@@   @@@                                                                               @@@                     @@@@@@     @@@@@@@@@@@    @@@@@@@                    @@@                                     
                                        @ @ @@@ @@ @                                                        @@@@ @@@@@@@                                                                               @@@                     @@@@@@     @@ @@@@  @@    @@@@@@@                    @@@@                                    
                                        @@@@@ @@@@@@@   @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@   @ @@@@@ @@@@                                                                               @@@                   @@@@@@@@     @@@@@@@@@@@    @@@@@@@@@                   @@@                                    
                                        @@@ @@    @ @@ @@@                                             @@  @@ @   @@@@@@                                                                              @@@@                @@@@@ @@@@@        @@@@        @@@@@@@@@@@                 @@@                                    
                                        @@@@@      @ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @ @@      @@@@@                                                                             @ @               @@@@@ @@@@@@@      @@@@@@@@@     @@@@@@@@ @@@@@              @ @                                    
                                        @@@@@@@@@@@@@@@  @                                            @ @@ @@@@@@@@@@@@@@                                                                             @@@      @@@@@@@@@@@@@@@@@@@@@@      @ @@@@@ @     @@@@@@@ @@@@@@@@@@@@@@      @@@@                                   
                                       @@@@@@@@@@@@@@@   @                                            @ @@ @ @@@@@@@@ @@@                                                                             @@@      @@      @@@ @@@  @@@@@      @@@@@@@@@     @@@@@   @@@ @@@     @@      @@@@                                   
                                       @@@@@@@@   @@@@   @                                            @ @@ @ @@   @@@ @@@@                                                                            @ @      @@@@@@@@@@@@@@@@@@@@@@        @@@@@       @@@@@@@@@@@@@@@@@@@@@@       @@@                                   
                                       @@  @@@@@@@@@@@   @                                            @ @@ @ @@@@@@@@  @ @                                                                           @@@@              @@@@@@@@@@@@@@        @@@@        @@@@@@@@@@@@@@@              @@@                                   
                                       @@@ @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@@  @@@                                                                           @@@                 @@@@@@@@@@@@        @@@@@@      @@@@@@@@@@@@                 @ @                                   
                                       @ @ @   @@@   @   @                                            @ @@ @   @@@     @@@                                                                           @@@                  @@ @@@@@ @@        @@@@        @@@@@@@@@@@                  @ @                                   
                                       @   @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@   @ @                                                                           @ @                  @@@@@@@@ @@@       @@@@       @@@@@@@@@@@@                  @@@@                                  
                                      @@@@ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@  @@@@                                                                          @@@                  @@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@                 @@@@                                  
                                      @@@@ @@@@   @@@@   @                                            @ @@ @@@@   @@@   @@@                                                                          @@@                  @@@@@@@@@@@ @@@@   @@@@   @@@@@@@@@ @@@@@@@                  @ @                                  
                                      @@@@ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@   @@@                                                                         @@@@                  @@@@@ @@@@@@@ @@@@@@@@@@@@@@  @@@@@@@@@@@@@                  @@@                                  
                                      @@@@ @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@@   @ @                                                                         @@@                  @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@  @@@                 @ @                                  
                                      @ @@ @         @   @                                            @ @@ @            @@@                                                                         @@@                @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                @@@@                                 
                                     @@@@@ @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@@   @@@                                                                         @@@              @@@@ @@@@@@@@@@@   @@@@ @@@@@@@@@   @@@@@@@@@@@@@@@@              @@@@                                 
                                     @@@@@ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@   @@@@                                                                        @ @              @@@@@@@              @@@@@@@@@@@             @@@@ @@               @ @                                 
                                     @ @ @ @@@    @@@@   @                                            @ @@ @@@@   @@@    @@@                                                                        @@@              @@@@@                 @@@@@@@@                 @@@@@@              @@@                                 
                                     @@@ @ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@    @@@                                                                       @@@               @@@@                    @@@@@                    @@@               @@@                                 
                                     @@@ @ @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@@    @@@                                                                       @ @                                       @@@@                                       @ @                                 
                                    @@@  @ @  @@@@   @   @                                            @ @@ @   @@@@      @ @                                                                       @@@                                       @@@@                                       @@@@                                
                                    @@@  @ @@@@@@@@@ @   @                                            @ @@ @ @@@@@@@@    @@@                                                                       @ @                   @@@ @@@             @@@@@@           @@@ @@@@                  @@@@                                
                                    @@@  @ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@     @@@                                                                     @ @                    @ @@@ @@@          @@@@@@@          @@ @@@@@@                @@@@ @                                
                                    @@@  @ @@@@  @@@@@   @                                            @ @@ @@@@   @@@     @ @                                                                     @@@@@@@                @@@@@@@ @          @ @ @ @        @@@@@@@@@@@                @@@@@@                                
                                   @@@@@@@ @@@@@@@@@@@   @                                            @ @@ @@@@@@@@@@     @@@                                                                     @@@@@@@                   @ @@@@@@@       @ @ @ @       @@ @@@@@@@ @                @@@@@@                                
                                   @@@@@@@ @@@@@@@@@ @   @                                            @ @@@@@@@@@@@@   @@@@@@                                                                     @@    @                @  @@@ @ @ @@@     @ @ @ @     @@@@@ @@@@ @ @               @@@@@@@@                               
                                   @@@@@@@ @  @@@@   @   @                                            @ @ @@@@@@@@@    @@@@@@                                                                    @@@@   @                @    @@@@@@@ @     @ @ @ @   @@@ @  @@@@  @ @               @@@@@@@@                               
                                   @@@@@@  @@@@@@@@@ @   @                                            @ @  @@@@@@@@@@  @@@@@@@                                                                   @ @@   @                @ @    @@ @@@@@@@  @ @ @ @  @@ @@@@@@@    @ @               @@  @  @                               
                                   @ @@@@  @@@@@@@@@@@   @                                            @ @   @@@@@@@@@  @@@@@@@                                                                   @@@@@  @                @ @     @@@ @ @ @@ @ @ @ @@@@@@ @ @@      @ @               @@  @@@@@                              
                                  @@@@@@@  @@@@  @@@@@   @                                            @ @   @@@   @@@  @@@@@@@                                                                   @@@@@ @@@               @ @       @@@@@@@@@@ @ @ @@ @ @ @@@       @ @               @@  @@@@@                              
                                  @@@ @@@  @@@@@@@@@@@   @                                            @ @   @@@@@@@@@  @@@@@@@                                                                   @ @@@ @@@               @ @         @@ @@@ @ @ @ @@@@@@@@         @ @               @@  @@@@@                              
                                  @@@@@@@  @@@@@@@@@@ @@ @                                            @ @    @@@@@@@   @@@@@@ @                                                                 @@@@@@ @@@               @ @          @@@ @@@   @@@@@ @            @ @              @@@  @@@@@                              
                                  @@@@@ @  @  @@@@@@ @@@ @                                            @ @     @@@@@    @@ @@                                                                    @@@@@@ @@@               @ @            @@@@@@  @@@ @@@            @ @              @@@   @  @                              
                                  @ @@@ @  @@@@@@@@@@  @ @                                            @ @    @@@@@@@@  @@ @@@@@                                                                 @@ @@@ @@@@              @ @              @@ @  @@ @@              @ @              @@    @@@@                              
                                  @ @@@ @  @@@@@@@@@@  @ @                                            @ @   @@@@ @@@@  @@ @@@ @                                                                 @ @@ @ @@@@              @ @               @@@@ @@@                @ @             @@@    @@@@@                             
                                 @@@@@@ @  @@@@  @@@@  @ @                                            @ @   @@@@ @@@@  @@ @@ @@@                                                                @@@@ @ @ @@              @ @                @ @ @ @                @ @             @@@    @@@@@                             
                                 @@@@@  @  @@@@@@@@@   @ @                                            @ @   @@@@@@@@@  @@ @@@@@@                                                                @@@@ @ @ @@              @ @                @ @ @ @                @ @             @@@     @  @                             
                                 @@@@@  @  @ @@@@@@    @ @                                            @ @   @@@@@@@   @@  @@@@@                                                               @@@@  @ @ @@@                                @ @ @ @                @ @             @@      @@@@                             
                                 @@ @@  @  @ @@@@@@    @ @                                            @ @   @@@@@    @@  @@@ @                                                               @@@@  @ @ @@@             @@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@             @@      @@@@                             
                                 @ @@@  @  @@@@@@@@@   @ @                                            @ @   @@@@@@@@  @@  @@@@@                                                               @@@@  @ @  @@   @@@@@@@@@@@@                 @@@ @@@                @@@ @@@@@@@@@  @@@       @@@@                            
                                @@@@@   @  @@@@@ @@@   @ @                                            @ @   @@@@ @@@@  @@  @@ @@                                                               @@@@  @ @  @@@@@@@@@ @@@  @@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@ @@  @@@ @   @@@       @@@@                            
                                @@@@@   @  @@@@  @@@   @ @                                            @ @   @@@@ @@@@  @@   @@@@@                                                              @@@   @ @  @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@@ @@        @@@@                            
                                @ @@@   @  @@@@@@@@@   @@@                                            @@@    @@@@@@@@  @@   @@@@@                                                              @@@   @ @  @@@ @@@@ @@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@        @@@@                            
                                @@@@@   @@@@ @@@@@@     @                                             @@@    @@@@@@@@@@@@   @@@@@                                                             @@@@   @ @  @@@@@@@@@@@@@@@@@@@@@@ @@@@@ @@@@@ @@@@ @@@@@@@ @  @ @@@@@@@@@@@@@@@@@@ @@         @@@                            
                                @@@@    @@@@@                                                                       @@@@@   @@@@@                                                             @@@@   @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@@     @@@                            
                                 @@@    @@@ @@@                                                                   @@@@@@@   @@@ @                                                             @@@@   @@@ @@@@@@@@@@@@@@@@@@@@@@@ @@        @@@@ @@@@       @@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@   @@@@                           
                               @@@@@    @@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@@    @@@@                                                             @@@  @@@@@@@@@@@@@@@@@@@@@ @@@@    @@@@@@@@@@@@@@@@@@@@@@@@@@@@    @@@@  @@@@@@@@@@@@@@@@@@@@@ @@@@                           
                               @@@@   @@@@@@@@@@@@@@        @@@                                   @@@        @@@@@@@@@@@@@@  @@@@@                                                            @@@@@@@@@@@         @@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  @@@@@@        @@@@@ @@  @@                           
                               @@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  @@ @                                                           @@@@@@@@@              @@@@@@@@@@@@@@@@@@@@@@@@@@  @@@@@@@@@@@@@@@@@@@@@ @@@@              @@@@@@@@@                           
                              @@@@@@@@@@@@@       @@@@@@@@@@@@@ @                               @ @ @@@@@@@@@@@      @@@@@@@@@@@@@                                                           @@@@@@@@                 @@@@@@@@                                   @@@@@@@                  @@@@@@@                           
                              @ @@@@@@@@             @@@@ @@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  @  @@@@             @@@@@@@@@                                                           @@@ @@@                   @@@@@ @                                   @ @@@@                    @@@@ @@                          
                              @@@@@@@@                 @@@@@@@@@                                 @@  @ @@                  @@@@@@@                                                          @@@@@@@                     @@@@ @                                   @ @@@@                     @@ @@@                          
                              @@@ @@@                    @@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ @@@@                    @@@@@@@                                                         @ @ @@                      @@@@ @                                   @ @@@                      @@ @@@                          
                              @ @@@@                     @@@@ @                                   @ @@@@                    @@@ @@@                                                         @@@ @@@                     @@ @ @                                   @ @@@@                    @@@ @@@                          
                             @@@@@@@                     @@ @ @                                   @ @@@@                     @@ @@@                                                         @@@@@@@@                   @@@ @ @                                   @ @@@@@                   @@@@@@@@                         
                             @@@@@@@                     @@ @ @                                   @ @@@@                     @@ @@@                                                         @ @@@@@@@                 @@@@@@ @                                   @ @@@@@@                @@@ @@@@ @                         
                             @@@@@@@@                   @@@@@ @                                   @ @@@@@                   @@@@ @ @                                                        @@@@@@@@@@@             @@@@@@@@ @                                   @ @@@@@@@@            @@@@@@@@@@@@                         
                             @@@@@@@@@                 @@@@@@ @                                   @ @@@@@@                 @@@@@  @                                                        @@@@@ @@@@@@@@@       @@@@@@@@@@@@@                                   @@@@ @@ @@@@@      @@@@@@@@@ @@@@@                         
                             @ @@@@@@@@               @@@@@@@ @                                   @ @@@@@@               @@@@@@@ @@@                                                       @ @@@  @@@@@@@@@@@@@@@@@@@@ @@@@@@@                                   @@@@@ @@@@@@@@@@@@@@@@@@@@@  @@@ @                         
                             @@@@@ @@@@@@@         @@@@@@@@@@ @                                   @ @@ @@@@@@@         @@@@ @@@@ @ @                                                       @ @@@     @@@@ @@@@@@@ @@@@@@@@@@@                                     @@@@@@@@@@@@@@@@@@@@@@@     @@@ @                         
                            @@@@@@  @@@@@@@@@@@@@@@@@@@@@@@@@@@                                   @@@@@@@@@@@@@@@@@@@@@@@@@@@ @@ @@@                                                       @ @@@@@@@     @@@@@@@@@@@@@@@@@                                          @@@@@@@ @@@@@@@@@     @@@@@@@@@                         
                            @@@@@@    @@@@@@@@@@@@@@@@@@@@@@@@@                                   @@@@@@@@@ @@@@@@@@@@@@@@@   @@  @@@                                                      @@@@@@@@@@@@@  @@@@@@ @@@@@@                                                @@@@@@@@@@@@@  @@@@@@@@@@@@@                         
                            @@@@@@@@@      @@@@@@@@@@@@@@@@@                                         @@@@@@@@@@@@@@@@@     @@@@@@@@ @                                                          @@@@@@@@@@@@@@ @@@@@@                                                      @@@@@@ @@@@@@@@@@@@@@@                            
                            @@@@@@@@@@@@@@   @@@@@ @@@@@@                                              @@@@@@@ @@@@@   @@@@@@@@@@@@@@                                                              @@@@@@@@@@@@@@                                                            @@@@@@@@@@@@@@@                                
                               @@@ @@@@@@@@@@@@@@ @@@@                                                     @@@@  @@@@@@@@@@@@@@@@@                                                                      @@@@@@                                                                  @@@@@@@                                     
                                   @@@ @@@@@@@@@@@@                                                           @@@@@@@@@@@@@@@@                                                                                                                                                                                              
                                       @@@@  @@@                                                                @@@@ @@@@@                                                                                                                                                                                                  
                                                                                                                    @                                                                                                                                                                                                       
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                            
)";
}

const char CONFIG_HTML[] PROGMEM =
R"html(
<!DOCTYPE html>
<html>
<head>
    <title>OUI-SPY Detector</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { box-sizing: border-box; }
        body { 
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
            margin: 0; 
            padding: 20px;
            background: #0f0f23; 
            color: #ffffff;
            position: relative;
            overflow-x: hidden;
        }
        .container {
            max-width: 700px; 
            margin: 0 auto; 
            background: rgba(255, 255, 255, 0.02); 
            padding: 40px; 
            border-radius: 16px; 
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.2); 
            backdrop-filter: blur(5px);
            border: 1px solid rgba(255, 255, 255, 0.05);
            position: relative;
            z-index: 1;
        }
        h1 {
            text-align: center;
            margin-bottom: 20px;
            margin-top: 0px;
            font-size: 48px;
            font-weight: 700;
            color: #8a2be2;
            background: -webkit-linear-gradient(45deg, #8a2be2, #4169e1);
            background: -moz-linear-gradient(45deg, #8a2be2, #4169e1);
            background: linear-gradient(45deg, #8a2be2, #4169e1);
            -webkit-background-clip: text;
            -moz-background-clip: text;
            background-clip: text;
            -webkit-text-fill-color: transparent;
            -moz-text-fill-color: transparent;
            letter-spacing: 3px;
        }
        @media (max-width: 768px) {
            h1 {
                font-size: clamp(32px, 8vw, 48px);
                letter-spacing: 2px;
                margin-bottom: 15px;
                text-align: center;
                display: block;
                width: 100%%;
            }
            .container {
                padding: 20px;
                margin: 10px;
            }
        }
        .section { 
            margin-bottom: 30px; 
            padding: 25px; 
            border: 1px solid rgba(255, 255, 255, 0.1); 
            border-radius: 12px; 
            background: rgba(255, 255, 255, 0.01); 
            backdrop-filter: blur(3px);
        }
        .section h3 { 
            margin-top: 0; 
            color: #ffffff; 
            font-size: 18px;
            font-weight: 600;
            margin-bottom: 15px;
        }
        textarea { 
            width: 100%%; 
            min-height: 120px;
            padding: 15px; 
            border: 1px solid rgba(255, 255, 255, 0.2); 
            border-radius: 8px; 
            background: rgba(255, 255, 255, 0.02);
            color: #ffffff;
            font-family: 'Courier New', monospace;
            font-size: 14px;
            resize: vertical;
        }
        textarea:focus {
            outline: none;
            border-color: #4ecdc4;
            box-shadow: 0 0 0 3px rgba(78, 205, 196, 0.2);
        }
        .help-text { 
            font-size: 13px; 
            color: #a0a0a0; 
            margin-top: 8px; 
            line-height: 1.4;
        }
        .toggle-container {
            display: flex;
            flex-direction: column;
            gap: 15px;
        }
        .toggle-item {
            display: flex;
            align-items: center;
            gap: 15px;
            padding: 15px;
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 8px;
            background: rgba(255, 255, 255, 0.02);
        }
        .toggle-item input[type="checkbox"] {
            width: 20px;
            height: 20px;
            accent-color: #4ecdc4;
            cursor: pointer;
        }
        .toggle-label {
            font-weight: 500;
            color: #ffffff;
            cursor: pointer;
            user-select: none;
        }
        button { 
            background: linear-gradient(135deg, #667eea 0%%, #764ba2 100%%); 
            color: #ffffff; 
            padding: 14px 28px; 
            border: none; 
            border-radius: 8px; 
            cursor: pointer; 
            font-size: 16px; 
            font-weight: 500;
            margin: 10px 5px; 
            transition: all 0.3s;
        }
        button:hover { 
            transform: translateY(-2px);
            box-shadow: 0 8px 25px rgba(102, 126, 234, 0.4);
        }
        .button-container {
            text-align: center;
            margin-top: 40px;
            padding-top: 30px;
            border-top: 1px solid #404040;
        }
        .status { 
            padding: 15px; 
            border-radius: 8px; 
            margin-bottom: 30px; 
            margin-top: 10px;
            border-left: 4px solid #ff1493;
            background: rgba(255, 20, 147, 0.05);
            color: #ffffff;
            border: 1px solid rgba(255, 20, 147, 0.2);
        }
        .oui-db details { margin-bottom: 8px; }
        .oui-db summary { cursor: pointer; padding: 10px; border-radius: 6px; background: rgba(255,255,255,0.03); transition: background 0.2s; }
        .oui-db summary:hover { background: rgba(255,255,255,0.07); }
        .oui-db .oui-entries { padding: 8px 0; line-height: 2; }
        .oui-db .oui-entries code { display: inline-block; margin: 2px 4px; padding: 2px 8px; background: rgba(78,205,196,0.1); border-radius: 4px; font-size: 12px; color: #4ecdc4; }
        .sig-lines { margin-top: 12px; }
        .sig-line {
            display: flex; align-items: center; flex-wrap: wrap; gap: 6px;
            padding: 8px 10px; margin-bottom: 6px;
            background: rgba(255,255,255,0.03);
            border: 1px solid rgba(255,255,255,0.10);
            border-radius: 8px; font-size: 12px;
            font-family: 'SFMono-Regular', Consolas, monospace;
        }
        .sig-vendor { font-weight: 700; color: #ffffff; margin-right: 4px;
                      font-family: 'Segoe UI', sans-serif; }
        /* one colour per signature class */
        .sig-mac  { color: #4dd0e1; }   /* OUI / MAC prefix   - cyan   */
        .sig-cid  { color: #ffb74d; }   /* BT SIG company ID  - amber  */
        .sig-uuid { color: #81c784; }   /* service UUID       - green  */
        .sig-name { color: #ba9ffb; }   /* name substring     - purple */
        .sig-meta { color: #e94560; }   /* Meta composite     - red-pink (matches META badge) */
        .sig-probe  { color: #4ecdc4; }   /* WiFi probe     - teal  */
        .sig-beacon { color: #4ecdc4; }   /* WiFi beacon    - teal  */
        .sig-sep  { color: #6b6b7d; }
        .sig-rm {
            margin-left: auto; background: none; border: none;
            color: #ff6b6b; cursor: pointer; font-size: 14px;
            padding: 0 4px; line-height: 1;
        }
        .oui-add-btn { background: linear-gradient(135deg, #10b981 0%%, #059669 100%%) !important; font-size: 13px !important; padding: 8px 16px !important; margin: 8px 0 !important; width: 100%%; }
        .oui-add-btn:hover { box-shadow: 0 4px 15px rgba(16, 185, 129, 0.4) !important; }
        .oui-db .oui-meta { font-size: 12px; color: #a0a0a0; margin: 2px 0; padding-left: 8px; }
        .oui-db .oui-note { font-size: 11px; color: #888; margin: 4px 0; padding-left: 8px; font-style: italic; }
        .oui-db .oui-note { font-size: 11px; color: #888; margin: 4px 0; padding-left: 8px; font-style: italic; }
    </style>
</head>
<body>
    <div class="container">
        <h1>OUI-SPY Detector</h1>
        
        <div class="status">
            Add OUI/MAC filters below using the form, or select presets from the OUI Database.
        </div>

        <form id="configForm" method="POST" action="/save" autocomplete="off">
            <input type="hidden" id="filtersJson" name="filters" value="">
            <div class="section">
                <h3>OUI Prefixes</h3>
                <textarea id="ouis" name="ouis" placeholder="Enter OUI prefixes, one per line:
AA:BB:CC
DD:EE:FF
11:22:33">%OUI_VALUES%</textarea>
                <div class="help-text">
                    OUI prefixes (first 3 bytes) match all devices from a manufacturer.<br>
                    Format: XX:XX:XX (8 characters with colons)
                </div>
                <div id="sigLines" class="sig-lines"></div>
            </div>

            <div id="filterList">%FILTER_LIST_ROWS%</div>
            <div id="emptyFilterMsg" class="empty-state-message">
                No filters configured. Select a preset from the OUI Database below.
            </div>

            <div class="section">
                <h3>OUI Database</h3>
                <div class="help-text" style="margin-bottom: 15px;">
                    Browse known surveillance device OUI prefixes by manufacturer. Click <strong>"+ Add"</strong> to append them to your filter list above.
                </div>
                <div class="oui-db">
                    <!-- OUI_DB_START -->
                    <details>
                    <summary><b>RING</b> <code>11 OUIs</code></summary>
                    <div class="oui-entries"><code>18:7F:88</code> <code>24:2B:D6</code> <code>34:3E:A4</code> <code>54:E0:19</code> <code>5C:47:5E</code> <code>64:9A:63</code> <code>90:48:6C</code> <code>9C:76:13</code> <code>AC:9F:C3</code> <code>C4:DB:AD</code> <code>CC:3B:FB</code></div>
                    <button type="button" class="oui-add-btn" onclick="addFilterRow('187F88',true,true,'Ring doorbell/camera');addFilterRow('242BD6',true,true,'Ring doorbell/camera');addFilterRow('343EA4',true,true,'Ring doorbell/camera');addFilterRow('54E019',true,true,'Ring doorbell/camera');addFilterRow('5C475E',true,true,'Ring doorbell/camera');addFilterRow('649A63',true,true,'Ring doorbell/camera');addFilterRow('90486C',true,true,'Ring doorbell/camera');addFilterRow('9C7613',true,true,'Ring doorbell/camera');addFilterRow('AC9FC3',true,true,'Ring doorbell/camera');addFilterRow('C4DBAD',true,true,'Ring doorbell/camera');addFilterRow('CC3BFB',true,true,'Ring doorbell/camera')">+ Add to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Doorbell/Security Camera</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> Typical WiFi/BLE range</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Ring Doorbell, Ring Camera, Ring Chime</div>
                    </details>
                    <details>
                    <summary><b>AXON</b> <code>1 OUI</code></summary>
                    <div class="oui-entries"><code>00:25:DF</code></div>
                    <button type="button" class="oui-add-btn" onclick="addVendor('axon', 'AXON', '00:25:DF')">+ Add to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Body Camera / Law Enforcement</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> Short-range BLE/WiFi</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Axon Body Camera, Axon Fleet</div>
                    </details>
                    <details>
                    <summary><b>I-PRO</b> <code>1 OUI</code></summary>
                    <div class="oui-entries"><code>D4:2D:C5</code></div>
                    <button type="button" class="oui-add-btn" onclick="addVendor('ipro', 'I-PRO', 'D4:2D:C5')">+ Add to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Body Camera / Law Enforcement</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> Short-range BLE/WiFi</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> I-PRO Body Camera</div>
                    </details>
                    <details>
                    <summary><b>FLOCK SAFETY</b> <code>1 OUI</code></summary>
                    <div class="oui-entries"><code>B4:1E:52</code></div>
                    <button type="button" class="oui-add-btn" onclick="addFilterRow('B41E52',true,true,'Flock Safety camera')">+ Add to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Automated License Plate Reader (ALPR) / Security Camera</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi/Cellular</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Flock Safety Camera, Falcon Camera, Raven Camera</div>
                    </details>
                    <details>
                    <summary><b>FLOCK SAFETY (WiFi Promiscuous — @NitekryDPaul research)</b> <code>30 OUIs</code></summary>
                    <div class="oui-entries"><code>70:C9:4E</code> <code>3C:91:80</code> <code>D8:F3:BC</code> <code>80:30:49</code> <code>B8:35:32</code> <code>14:5A:FC</code> <code>74:4C:A1</code> <code>08:3A:88</code> <code>9C:2F:9D</code> <code>C0:35:32</code> <code>94:08:53</code> <code>E4:AA:EA</code> <code>F4:6A:DD</code> <code>F8:A2:D6</code> <code>24:B2:B9</code> <code>00:F4:8D</code> <code>D0:39:57</code> <code>E8:D0:FC</code> <code>E0:4F:43</code> <code>B8:1E:A4</code> <code>70:08:94</code> <code>58:8E:81</code> <code>EC:1B:BD</code> <code>3C:71:BF</code> <code>58:00:E3</code> <code>90:35:EA</code> <code>5C:93:A2</code> <code>64:6E:69</code> <code>48:27:EA</code> <code>A4:CF:12</code></div>
                    <button type="button" class="oui-add-btn" onclick="addFilterRow('70C94E',true,true,'Flock Safety infrastructure');addFilterRow('3C9180',true,true,'Flock Safety infrastructure');addFilterRow('D8F3BC',true,true,'Flock Safety infrastructure');addFilterRow('803049',true,true,'Flock Safety infrastructure');addFilterRow('B83532',true,true,'Flock Safety infrastructure');addFilterRow('145AFC',true,true,'Flock Safety infrastructure');addFilterRow('744CA1',true,true,'Flock Safety infrastructure');addFilterRow('083A88',true,true,'Flock Safety infrastructure');addFilterRow('9C2F9D',true,true,'Flock Safety infrastructure');addFilterRow('C03532',true,true,'Flock Safety infrastructure');addFilterRow('940853',true,true,'Flock Safety infrastructure');addFilterRow('E4AAEA',true,true,'Flock Safety infrastructure');addFilterRow('F46ADD',true,true,'Flock Safety infrastructure');addFilterRow('F8A2D6',true,true,'Flock Safety infrastructure');addFilterRow('24B2B9',true,true,'Flock Safety infrastructure');addFilterRow('00F48D',true,true,'Flock Safety infrastructure');addFilterRow('D03957',true,true,'Flock Safety infrastructure');addFilterRow('E8D0FC',true,true,'Flock Safety infrastructure');addFilterRow('E04F43',true,true,'Flock Safety infrastructure');addFilterRow('B81EA4',true,true,'Flock Safety infrastructure');addFilterRow('700894',true,true,'Flock Safety infrastructure');addFilterRow('588E81',true,true,'Flock Safety infrastructure');addFilterRow('EC1BBD',true,true,'Flock Safety infrastructure');addFilterRow('3C71BF',true,true,'Flock Safety infrastructure');addFilterRow('5800E3',true,true,'Flock Safety infrastructure');addFilterRow('9035EA',true,true,'Flock Safety infrastructure');addFilterRow('5C93A2',true,true,'Flock Safety infrastructure');addFilterRow('646E69',true,true,'Flock Safety infrastructure');addFilterRow('4827EA',true,true,'Flock Safety infrastructure');addFilterRow('A4CF12',true,true,'Flock Safety infrastructure')">+ Add to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Automated License Plate Reader (ALPR) / Security Camera</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi 2.4 GHz (promiscuous mode, addr1 + addr2)</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Flock Safety infrastructure (cameras, uplinks, peripherals)</div>
                    <div class="oui-meta"><strong>Research Credit:</strong> ØяĐöØцяöЪöяцฐ / <strong>@NitekryDPaul</strong> — identified these prefixes through 2.4 GHz promiscuous-mode analysis, including the addr1-receiver detection technique that catches Flock stations during their burst-sleep duty cycle</div>
                    </details>
                    <details>
                    <summary><b>DJI</b> <code>8 OUIs</code></summary>
                    <div class="oui-entries"><code>0C:9A:E6</code> <code>8C:58:23</code> <code>04:A8:5A</code> <code>58:B8:58</code> <code>E4:7A:2C</code> <code>60:60:1F</code> <code>48:1C:B9</code> <code>34:D2:62</code></div>
                    <button type="button" class="oui-add-btn" onclick="addFilterRow('0C9AE6',true,true,'DJI drone');addFilterRow('8C5823',true,true,'DJI drone');addFilterRow('04A85A',true,true,'DJI drone');addFilterRow('58B858',true,true,'DJI drone');addFilterRow('E47A2C',true,true,'DJI drone');addFilterRow('60601F',true,true,'DJI drone');addFilterRow('481CB9',true,true,'DJI drone');addFilterRow('34D262',true,true,'DJI drone')">+ Add to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Consumer & Commercial Drones</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi/OcuSync up to several km</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Mavic, Phantom, Inspire, Mini series</div>
                    </details>
                    <details>
                    <summary><b>PARROT</b> <code>5 OUIs</code></summary>
                    <div class="oui-entries"><code>00:12:1C</code> <code>00:26:7E</code> <code>90:03:B7</code> <code>90:3A:E6</code> <code>A0:14:3D</code></div>
                    <button type="button" class="oui-add-btn" onclick="addFilterRow('00121C',true,true,'Parrot drone');addFilterRow('00267E',true,true,'Parrot drone');addFilterRow('9003B7',true,true,'Parrot drone');addFilterRow('903AE6',true,true,'Parrot drone');addFilterRow('A0143D',true,true,'Parrot drone')">+ Add to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Consumer & Commercial Drones</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi/BLE range</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Parrot Anafi, Parrot Bebop, Parrot AR.Drone</div>
                    </details>
                    <details>
                    <summary><b>SKYDIO</b> <code>1 OUI</code></summary>
                    <div class="oui-entries"><code>38:1D:14</code></div>
                    <button type="button" class="oui-add-btn" onclick="addFilterRow('381D14',true,true,'Skydio drone')">+ Add to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Commercial & Enterprise Drones</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi range</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Skydio 2, Skydio X2, Skydio 3</div>
                    </details>
                    <details>
                    <summary><b>META/RAYBAN SMARTGLASSES</b> <code>5 OUIs</code></summary>
                    <div class="oui-note"><em>sourced from <a href=\"https://github.com/sh4d0wm45k/glass-detect/blob/main/glass-detect/glass-detect.ino#L21\" target=\"_blank\" style=\"color:#4ecdc4;\">glass-detect repository</a></em></div>
                    <div class="oui-note">Last one from Luxottica Group S.P.A. detected by @konradit.</div>
                    <div class="oui-entries"><code>7C:2A:9E</code> <code>CC:66:0A</code> <code>F4:03:43</code> <code>5C:E9:1E</code> <code>98:59:49</code></div>
                    <button type="button" class="oui-add-btn" onclick="addFilterRow('7C2A9E',true,true,'Meta/Ray-Ban Smartglasses');addFilterRow('CC660A',true,true,'Meta/Ray-Ban Smartglasses');addFilterRow('F40343',true,true,'Meta/Ray-Ban Smartglasses');addFilterRow('5CE91E',true,true,'Meta/Ray-Ban Smartglasses');addFilterRow('985949',true,true,'Meta/Ray-Ban Smartglasses')">+ Add to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Smartglasses</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi/BLE range</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Meta/Ray-Ban Smartglasses</div>
                    </details>
                    <!-- OUI_DB_END -->
                </div>
            </div>


            <div class="section">
                <h3>Audio & Visual Settings</h3>
                <div class="toggle-container">
                    <div class="toggle-item">
                        <input type="checkbox" id="buzzerEnabled" name="buzzerEnabled" %BUZZER_CHECKED%>
                        <label class="toggle-label" for="buzzerEnabled">Enable Buzzer</label>
                        <div class="help-text" style="margin-top: 0;">Audio feedback for target detection</div>
                    </div>
                    <div class="toggle-item">
                        <input type="checkbox" id="ledEnabled" name="ledEnabled" %LED_CHECKED%>
                        <label class="toggle-label" for="ledEnabled">Enable LED Blinking</label>
                        <div class="help-text" style="margin-top: 0;">Orange LED blinks with same pattern as buzzer</div>
                    </div>
                </div>
            </div>
            
            <div class="section">
                <h3>WiFi Access Point Settings</h3>
                <div class="help-text" style="margin-bottom: 15px;">
                    Customize the WiFi network name and password for the configuration portal.<br>
                    <strong>Changes take effect on next device boot.</strong>
                </div>
                <div style="margin-bottom: 15px;">
                    <label for="ap_ssid" style="display: block; margin-bottom: 8px; font-weight: 500; color: #ffffff;">Network Name (SSID)</label>
                    <input type="text" id="ap_ssid" name="ap_ssid" value="%AP_SSID%" maxlength="32" autocomplete="off" style="width: 100%%; padding: 12px; border: 1px solid rgba(255, 255, 255, 0.2); border-radius: 8px; background: rgba(255, 255, 255, 0.02); color: #ffffff; font-size: 14px;">
                    <div class="help-text" style="margin-top: 5px;">1-32 characters</div>
                </div>
                <div>
                    <label for="ap_password" style="display: block; margin-bottom: 8px; font-weight: 500; color: #ffffff;">Password</label>
                    <input type="text" id="ap_password" name="ap_password" value="%AP_PASSWORD%" minlength="8" maxlength="63" autocomplete="new-password" style="width: 100%%; padding: 12px; border: 1px solid rgba(255, 255, 255, 0.2); border-radius: 8px; background: rgba(255, 255, 255, 0.02); color: #ffffff; font-size: 14px;">
                    <div class="help-text" style="margin-top: 5px;">8-63 characters (leave empty for open network)</div>
                </div>
            </div>
            
            <!-- Detected Devices Section -->
            <div class="section" id="detectedDevicesSection">
                <h3>Device Alias Management</h3>
                <div class="help-text" style="margin-bottom: 15px;">
                    Assign identification labels to detected MAC addresses for serial output tracking.<br>
                    <strong>Device history and aliases persist in non-volatile storage.</strong>
                </div>
                <div id="clearDeviceBtn" style="margin-bottom: 10px; text-align: right; display: none;">
                    <button type="button" onclick="clearDeviceHistory()" style="background: #8b0000; padding: 8px 16px; font-size: 13px; margin: 0;">Clear Device History</button>
                </div>
                <div id="deviceList" class="device-list">
                    <div style="text-align: center; padding: 30px; color: #888888;">
                        <p style="font-size: 14px;">No device records in storage.</p>
                        <p style="font-size: 12px; margin-top: 10px;">Detected devices during scanning operations will persist to this list.</p>
                    </div>
                </div>
            </div>

            <div class="section">
                <h3>MQTT</h3>
                <div class="help-text" style="margin-bottom:15px">Publish detections to MQTT broker on your network.</div>
                <div class="toggle-item" style="margin-bottom:15px">
                    <input type="checkbox" id="mqtt_en" name="mqtt_en" %MQTT_EN%>
                    <label class="toggle-label" for="mqtt_en">Enable MQTT</label>
                </div>
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">Device ID</label>
                <input type="text" name="mq_id" value="%MQ_ID%" maxlength="32" placeholder="ouispy" style="width:100%%;padding:10px;margin-bottom:12px;border:1px solid rgba(255,255,255,0.2);border-radius:8px;background:rgba(255,255,255,0.02);color:#fff;font-size:14px">
                <div class="help-text" style="margin-bottom:12px">Unique name for this device (e.g. ouispy-front, ouispy-garage). Used as MQTT client ID and auto-generates topic.</div>
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">WiFi SSID</label>
                <input type="text" name="mq_ss" value="%MQ_SS%" maxlength="32" placeholder="Home WiFi" autocomplete="off" style="width:100%%;padding:10px;margin-bottom:12px;border:1px solid rgba(255,255,255,0.2);border-radius:8px;background:rgba(255,255,255,0.02);color:#fff;font-size:14px">
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">WiFi Password</label>
                <input type="password" name="mq_sp" value="%MQ_SP%" maxlength="63" placeholder="Password" autocomplete="new-password" style="width:100%%;padding:10px;margin-bottom:12px;border:1px solid rgba(255,255,255,0.2);border-radius:8px;background:rgba(255,255,255,0.02);color:#fff;font-size:14px">
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">Broker IP</label>
                <input type="text" name="mq_bk" value="%MQ_BK%" maxlength="64" placeholder="192.168.1.100" style="width:100%%;padding:10px;margin-bottom:12px;border:1px solid rgba(255,255,255,0.2);border-radius:8px;background:rgba(255,255,255,0.02);color:#fff;font-size:14px">
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">Port</label>
                <input type="number" name="mq_pt" value="%MQ_PT%" min="1" max="65535" style="width:100%%;padding:10px;margin-bottom:12px;border:1px solid rgba(255,255,255,0.2);border-radius:8px;background:rgba(255,255,255,0.02);color:#fff;font-size:14px">
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">Username (optional)</label>
                <input type="text" name="mq_us" value="%MQ_US%" maxlength="64" placeholder="Leave blank if none" style="width:100%%;padding:10px;margin-bottom:12px;border:1px solid rgba(255,255,255,0.2);border-radius:8px;background:rgba(255,255,255,0.02);color:#fff;font-size:14px">
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">Password (optional)</label>
                <input type="password" name="mq_pw" value="%MQ_PW%" maxlength="64" placeholder="Leave blank if none" autocomplete="new-password" style="width:100%%;padding:10px;margin-bottom:12px;border:1px solid rgba(255,255,255,0.2);border-radius:8px;background:rgba(255,255,255,0.02);color:#fff;font-size:14px">
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">Topic</label>
                <input type="text" name="mq_tp" value="%MQ_TP%" maxlength="128" placeholder="ouispy/detection" style="width:100%%;padding:10px;border:1px solid rgba(255,255,255,0.2);border-radius:8px;background:rgba(255,255,255,0.02);color:#fff;font-size:14px">
            </div>

            <div class="button-container">
                <button type="submit">Save Configuration & Start Scanning</button>
                <button type="button" onclick="clearConfig()" style="background: #8b0000; margin-left: 20px;">Clear All Filters</button>
                <button type="button" onclick="deviceReset()" style="background: #4a0000; margin-left: 20px; font-size: 12px;">Device Reset</button>
            </div>
            
            <!-- Burn In Configuration Section -->
            <div class="section" style="border: 2px solid #8b0000; background: linear-gradient(135deg, rgba(139, 0, 0, 0.03) 0%%, rgba(139, 0, 0, 0.08) 100%%); margin-top: 40px;">
                <h3 style="color: #ff6b6b; margin-top: 0; font-size: 18px; letter-spacing: 1px; text-transform: uppercase; border-bottom: 2px solid rgba(255, 107, 107, 0.3); padding-bottom: 12px; margin-bottom: 20px; text-align: center;">
                    Burn In Settings
                </h3>
                
                <div style="background: linear-gradient(135deg, #1a0a0a 0%%, #2d0a0a 100%%); color: #ff9999; padding: 18px; border-radius: 8px; margin: 15px 0; border: 2px solid #8b0000; box-shadow: 0 4px 15px rgba(139, 0, 0, 0.3);">
                    <p style="font-weight: 600; font-size: 13px; margin: 0 0 10px 0; color: #ff6b6b; text-transform: uppercase; letter-spacing: 0.5px;">
                        Warning - Requires Flash Erase to Unlock
                    </p>
                    <p style="line-height: 1.5; margin: 0 0 12px 0; color: #ffcccc; font-size: 13px;">
                        Permanently locks all current settings: <strong>OUI/MAC filters, device aliases, buzzer/LED preferences</strong>
                    </p>
                    <p style="line-height: 1.4; margin: 0 0 8px 0; color: #e0e0e0; font-weight: 500; font-size: 12px;">
                        Effects after activation:
                    </p>
                    <ul style="text-align: left; line-height: 1.6; margin: 0 0 12px 0; padding-left: 20px; color: #e0e0e0; font-size: 12px;">
                        <li>Disables WiFi AP and 20-second config window</li>
                        <li>Boots directly to scanning mode (~2 seconds)</li>
                        <li>Removes web interface access</li>
                    </ul>
                    <p style="line-height: 1.4; margin: 0; color: #ffcccc; font-size: 12px;">
                        <strong>Unlock:</strong> hold the BOOT button during power-on (or erase flash + reflash)
                    </p>
                </div>
                
                <div style="background: linear-gradient(135deg, #0a1a0a 0%%, #0a2d0a 100%%); color: #99ff99; padding: 18px; border-radius: 8px; margin: 15px 0; border: 1px solid #166534; box-shadow: 0 2px 10px rgba(22, 101, 52, 0.2);">
                    <p style="font-weight: 600; margin: 0 0 8px 0; color: #4ade80; font-size: 13px; text-transform: uppercase; letter-spacing: 0.5px;">
                        Use Cases:
                    </p>
                    <ul style="text-align: left; line-height: 1.6; margin: 0; padding-left: 20px; color: #ccffcc; font-size: 12px;">
                        <li>Production deployments</li>
                        <li>Fixed installations</li>
                        <li>Security-sensitive environments</li>
                        <li>Battery-powered optimization</li>
                    </ul>
                </div>
                
                <div style="text-align: center; margin-top: 25px; padding-top: 20px; border-top: 1px solid rgba(255, 107, 107, 0.2);">
                    <button type="button" onclick="burnInConfig()" style="background: linear-gradient(135deg, #8b0000 0%%, #6b0000 100%%); color: #ffffff; font-size: 15px; padding: 15px 35px; font-weight: 600; border: 2px solid #ff0000; border-radius: 8px; cursor: pointer; text-transform: uppercase; letter-spacing: 1px; box-shadow: 0 4px 15px rgba(139, 0, 0, 0.4); transition: all 0.3s;">
                        Lock Configuration Permanently
                    </button>
                    <p style="font-size: 11px; color: #888888; margin-top: 12px; font-style: italic;">
                        Undo by holding BOOT during power-on
                    </p>
                </div>
            </div>
            
            <style>
                .device-list {
                    display: flex;
                    flex-direction: column;
                    gap: 10px;
                    max-height: 400px;
                    overflow-y: auto;
                }
                .device-item {
                    display: flex;
                    flex-direction: column;
                    gap: 10px;
                    padding: 12px;
                    border: 1px solid rgba(255, 255, 255, 0.1);
                    border-radius: 8px;
                    background: rgba(255, 255, 255, 0.02);
                }
                .device-info-row {
                    display: flex;
                    align-items: center;
                    gap: 12px;
                    flex-wrap: wrap;
                }
                .device-alias-row {
                    display: flex;
                    align-items: center;
                    gap: 10px;
                    width: 100%%;
                }
                .device-mac {
                    font-family: 'Courier New', monospace;
                    font-weight: 500;
                    color: #4ecdc4;
                    font-size: 13px;
                }
                .device-rssi {
                    color: #a0a0a0;
                    font-size: 12px;
                }
                .device-time {
                    color: #888888;
                    font-size: 11px;
                    font-style: italic;
                }
                .device-time.recent {
                    color: #4ade80;
                }
                .alias-input {
                    flex: 1;
                    padding: 8px 12px;
                    border: 1px solid rgba(255, 255, 255, 0.2);
                    border-radius: 6px;
                    background: rgba(255, 255, 255, 0.05);
                    color: #ffffff;
                    font-size: 14px;
                    min-width: 0;
                }
                .alias-input:focus {
                    outline: none;
                    border-color: #4ecdc4;
                    box-shadow: 0 0 0 2px rgba(78, 205, 196, 0.2);
                }
                .save-alias-btn {
                    padding: 8px 16px;
                    font-size: 13px;
                    margin: 0;
                    white-space: nowrap;
                }
                .device-filter {
                    color: #a0a0a0;
                    font-size: 11px;
                    font-style: italic;
                }
                .match-badge {
                    display: inline-block;
                    padding: 2px 7px;
                    border-radius: 10px;
                    font-family: 'Courier New', monospace;
                    font-size: 10px;
                    font-weight: 700;
                    letter-spacing: 0.5px;
                    background: rgba(0,0,0,0.35);
                    border: 1px solid currentColor;
                    text-shadow: 0 0 4px currentColor;
                }
                .match-badge.type-OUI  { color: #00d4ff; }
                .match-badge.type-MAC  { color: #ff2ee0; }
                .match-badge.type-CID  { color: #ffb020; }
                .match-badge.type-SVC  { color: #a3ff2e; }
                .match-badge.type-NAME { color: #ff6b9d; }
                .match-badge.type-META { color: #e94560; }
                .match-badge.type-PROBE  { background: rgba(78,205,196,0.15); color: #4ecdc4; border-color: rgba(78,205,196,0.3); }
                .match-badge.type-BEACON { background: rgba(78,205,196,0.15); color: #4ecdc4; border-color: rgba(78,205,196,0.3); }
                .match-badge.type-BLE { color: #00d4ff; }
                .match-badge.type-WiFi { color: #4ecdc4; }

                /* Filter row styles */
                .filter-row {
                    display: flex; flex-wrap: wrap; align-items: center; gap: 10px;
                    padding: 10px 12px; margin-bottom: 6px;
                    border: 1px solid rgba(255,255,255,0.10);
                    border-bottom: 1px solid rgba(255,255,255,0.10);
                    border-radius: 8px; background: rgba(255,255,255,0.02);
                    font-size: 13px;
                }
                .filter-domains {
                    display: flex; align-items: center; gap: 6px; flex-wrap: wrap;
                }
                .filter-domains .match-badge {
                    cursor: pointer; min-height: 44px; display: inline-flex; align-items: center;
                }
                .filter-domains .match-badge:hover { opacity: 0.7; }
                .match-badge.disabled { color: #555555; border-color: #555555; opacity: 0.5; }
                .remove-filter-btn {
                    background: #8b0000; color: #ffffff; border: none;
                    border-radius: 6px; padding: 6px 12px; cursor: pointer;
                    font-size: 14px; line-height: 1; min-height: 44px; min-width: 44px;
                    display: inline-flex; align-items: center; justify-content: center;
                }
                .remove-filter-btn:hover { background: #a00000; }
                .preset-only { opacity: 0.55; }
                .empty-state-message {
                    text-align: center; padding: 30px; color: #888888; font-size: 14px;
                }
                .prev-tag {
                    display: inline-block;
                    margin-left: 6px;
                    padding: 1px 6px;
                    border-radius: 4px;
                    font-family: 'Courier New', monospace;
                    font-size: 9px;
                    letter-spacing: 0.5px;
                    color: #888;
                    border: 1px solid rgba(255,255,255,0.15);
                    background: rgba(255,255,255,0.03);
                }
            </style>
            
            <script>
            // Load detected devices on page load
            window.addEventListener('DOMContentLoaded', function() {
                loadDetectedDevices();
                
                // Ensure form submits on first click (mobile fix)
                const configForm = document.getElementById('configForm');
                if (configForm) {
                    const submitBtn = configForm.querySelector('button[type="submit"]');
                    if (submitBtn) {
                        submitBtn.addEventListener('touchstart', function(e) {
                            // Blur any focused inputs to ensure submit works on first tap
                            if (document.activeElement) {
                                document.activeElement.blur();
                            }
                        }, { passive: true });
                        
                        submitBtn.addEventListener('click', function(e) {
                            // Ensure any focused element is blurred before submit
                            if (document.activeElement && document.activeElement !== submitBtn) {
                                document.activeElement.blur();
                            }
                        });
                    }
                }
            });
            
            function formatTimeSince(milliseconds) {
                const seconds = Math.floor(milliseconds / 1000);
                const minutes = Math.floor(seconds / 60);
                const hours = Math.floor(minutes / 60);
                const days = Math.floor(hours / 24);
                
                if (seconds < 60) return 'Just now';
                if (minutes < 60) return minutes + ' min ago';
                if (hours < 24) return hours + ' hour' + (hours > 1 ? 's' : '') + ' ago';
                return days + ' day' + (days > 1 ? 's' : '') + ' ago';
            }
            
            function loadDetectedDevices() {
                fetch('/api/devices')
                    .then(response => response.json())
                    .then(data => {
                        const deviceList = document.getElementById('deviceList');
                        const clearBtn = document.getElementById('clearDeviceBtn');
                        
                        if (data.devices && data.devices.length > 0) {
                            clearBtn.style.display = 'block';
                            deviceList.innerHTML = '';
                            
                            data.devices.forEach(device => {
                                const deviceItem = document.createElement('div');
                                deviceItem.className = 'device-item';
                                
                                // First row: device info
                                const infoRow = document.createElement('div');
                                infoRow.className = 'device-info-row';
                                
                                const macSpan = document.createElement('span');
                                macSpan.className = 'device-mac';
                                macSpan.textContent = device.mac;
                                
                                const rssiSpan = document.createElement('span');
                                rssiSpan.className = 'device-rssi';
                                rssiSpan.textContent = device.rssi + ' dBm';
                                
                                const timeSpan = document.createElement('span');
                                timeSpan.className = 'device-time';
                                const timeSince = device.timeSince || 0;
                                timeSpan.textContent = formatTimeSince(timeSince);
                                if (timeSince < 60000) { // Less than 1 minute
                                    timeSpan.classList.add('recent');
                                }
                                
                                infoRow.appendChild(macSpan);
                                if (device.type) {
                                    infoRow.appendChild(makeMatchBadge(device.type, device.filter, device.match));
                                }
                                infoRow.appendChild(rssiSpan);
                                infoRow.appendChild(timeSpan);

                                if (device.filter) {
                                    const filterSpan = document.createElement('span');
                                    filterSpan.className = 'device-filter';
                                    filterSpan.textContent = device.filter;
                                    filterSpan.title = device.filter;
                                    infoRow.appendChild(filterSpan);
                                }
                                
                                // Second row: alias input and button
                                const aliasRow = document.createElement('div');
                                aliasRow.className = 'device-alias-row';
                                
                                const aliasInput = document.createElement('input');
                                aliasInput.type = 'text';
                                aliasInput.className = 'alias-input';
                                aliasInput.placeholder = 'Device identification label';
                                aliasInput.value = device.alias || '';
                                aliasInput.maxLength = 32;
                                
                                const saveBtn = document.createElement('button');
                                saveBtn.type = 'button';
                                saveBtn.className = 'save-alias-btn';
                                saveBtn.textContent = 'Save';
                                saveBtn.onclick = function() {
                                    saveAlias(device.mac, aliasInput.value, saveBtn);
                                };
                                
                                aliasRow.appendChild(aliasInput);
                                aliasRow.appendChild(saveBtn);
                                
                                deviceItem.appendChild(infoRow);
                                deviceItem.appendChild(aliasRow);
                                
                                deviceList.appendChild(deviceItem);
                            });
                        }
                    })
                    .catch(error => {
                        console.error('Error loading devices:', error);
                    });
            }
            
            function makeMatchBadge(type, description, matchIdent) {
                var badge = document.createElement('span');
                var allowed = ['OUI','MAC','CID','SVC','NAME','META','PROBE','BEACON'];
                var t = (allowed.indexOf(type) >= 0) ? type : 'OUI';
                badge.className = 'match-badge type-' + t;
                badge.textContent = t;
                var titleParts = [];
                if (matchIdent) titleParts.push(matchIdent);
                if (description) titleParts.push(description);
                badge.title = titleParts.join(' - ');
                return badge;
            }

            function saveAlias(mac, alias, button) {
                const originalText = button.textContent;
                const originalBg = button.style.background;
                button.textContent = 'Saving...';
                button.disabled = true;
                button.style.opacity = '0.6';
                
                fetch('/api/alias', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/x-www-form-urlencoded',
                    },
                    body: 'mac=' + encodeURIComponent(mac) + '&alias=' + encodeURIComponent(alias)
                })
                .then(response => response.json())
                .then(data => {
                    button.textContent = 'Saved!';
                    button.style.background = 'linear-gradient(135deg, #10b981 0%%, #059669 100%%)';
                    button.style.opacity = '1';
                    setTimeout(() => {
                        button.textContent = originalText;
                        button.style.background = originalBg;
                        button.disabled = false;
                    }, 2000);
                })
                .catch(error => {
                    console.error('Error saving alias:', error);
                    button.textContent = 'Error';
                    button.style.background = 'linear-gradient(135deg, #ef4444 0%%, #dc2626 100%%)';
                    button.style.opacity = '1';
                    setTimeout(() => {
                        button.textContent = originalText;
                        button.style.background = originalBg;
                        button.disabled = false;
                    }, 2000);
                });
            }
            
            function clearDeviceHistory() {
                if (confirm('CLEAR DEVICE HISTORY\n\nThis will remove all detected device records from non-volatile storage.\n\nAliases and filter configurations will be preserved.\n\nProceed with clearing device history?')) {
                    fetch('/api/clear-devices', { method: 'POST' })
                        .then(response => response.json())
                        .then(data => {
                            alert('Device history cleared from storage.');
                            location.reload();
                        })
                        .catch(error => {
                            console.error('Error:', error);
                            alert('Error clearing device history.');
                        });
                }
            }
            
            function applyPreset(name, label) {
                var statusEl = document.getElementById('presetStatus');
                statusEl.textContent = 'Adding ' + label + '…';
                statusEl.style.color = '#aaddff';
                var body = 'name=' + encodeURIComponent(name);
                fetch('/api/presets/apply', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: body
                })
                .then(function(r) { return r.json(); })
                .then(function(data) {
                    if (data.ok) {
                        statusEl.style.color = '#88ee88';
                        if (data.added > 0) {
                            statusEl.textContent = 'Added ' + data.added + ' new filter(s) for ' + label + '. Total filters: ' + data.total_filters + '.';
                        } else {
                            statusEl.textContent = label + ' preset already installed — no changes.';
                        }
                    } else {
                        statusEl.style.color = '#ff7777';
                        statusEl.textContent = 'Error: ' + (data.error || 'unknown');
                    }
                })
                .catch(function(err) {
                    statusEl.style.color = '#ff7777';
                    statusEl.textContent = 'Preset request failed: ' + err;
                });
            }

            function clearConfig() {
                if (confirm('Are you sure you want to clear all filters? This action cannot be undone.')) {
                    document.getElementById('filterList').innerHTML = '';
                    document.getElementById('filtersJson').value = '';
                    dirtyState = false;
                    showEmptyState();
                    fetch('/clear', { method: 'POST' })
                        .then(response => response.text())
                        .then(data => {
                            alert('All filters cleared!');
                            location.reload();
                        })
                        .catch(error => {
                            console.error('Error:', error);
                            alert('Error clearing filters. Check console.');
                        });
                }
            }
            
            function deviceReset() {
                if (confirm('DEVICE RESET: This will completely wipe all saved data and restart the device. Are you absolutely sure?')) {
                    if (confirm('This action cannot be undone. The device will restart and behave like first boot. Continue?')) {
                        fetch('/device-reset', { method: 'POST' })
                            .then(response => response.text())
                            .then(data => {
                                alert('Device reset initiated! Device restarting...');
                                setTimeout(function() {
                                    window.location.href = '/';
                                }, 5000);
                            })
                            .catch(error => {
                                console.error('Error:', error);
                                alert('Error during device reset. Check console.');
                            });
                    }
                }
            }
            
            // Signature sets that cannot be expressed via manual input.
            var VENDOR_OUIS = {
                axon:   '00:25:DF',
                ipro:   'D4:2D:C5',
                meta:   '7C:2A:9E,CC:66:0A,F4:03:43,5C:E9:1E,98:59:49'
            };
            var VENDOR_LABELS = { axon: 'AXON', meta: 'META / RAY-BAN', ipro: 'I-PRO' };

            // Repopulate the signature lines on page load. Without this the
            // filters stay installed in NVS but the UI looks empty after a
            // refresh.
            function loadSigLines() {
                fetch('/api/presets/status')
                    .then(function(r){ return r.json(); })
                    .then(function(d){
                        Object.keys(VENDOR_LABELS).forEach(function(k){
                            if (d && d[k]) {
                                renderSigLine(k, VENDOR_LABELS[k], VENDOR_OUIS[k], null);
                            }
                        });
                    })
                    .catch(function(){ /* device offline: leave lines empty */ });
            }

            

            function addVendor(preset, label, ouiStr) {
                if (ouiStr) {
                    addFilterRow(ouiStr.replace(':','').toUpperCase(), true, true, label);
                }
                fetch('/api/presets/apply', {
                    method: 'POST',
                    headers: {'Content-Type':'application/x-www-form-urlencoded'},
                    body: 'name=' + encodeURIComponent(preset)
                })
                .then(function(r){ return r.json(); })
                .then(function(d){
                    renderSigLine(preset, label, ouiStr, d && d.ok ? null : (d && d.error) || 'failed');
                })
                .catch(function(e){ renderSigLine(preset, label, ouiStr, String(e)); });
            }

            function renderSigLine(preset, label, ouiStr, err) {
                var box = document.getElementById('sigLines');
                if (!box) return;
                var existing = document.getElementById('sig-' + preset);
                if (existing) existing.remove();

                var parts = [];
                (ouiStr ? ouiStr.split(',') : []).forEach(function(o){
                    parts.push('<span class="sig-mac">' + o.trim() + '</span>');
                });


                var row = document.createElement('div');
                row.className = 'sig-line';
                row.id = 'sig-' + preset;
                row.innerHTML =
                    '<span class="sig-vendor">' + label + '</span>' +
                    parts.join('<span class="sig-sep">,</span> ') +
                    (err ? ' <span style="color:#ff6b6b">(' + err + ')</span>' : '') +
                    '<button type="button" class="sig-rm" title="Remove signatures" ' +
                        'onclick="removeVendor(\'' + preset + '\')">&times;</button>';
                box.appendChild(row);
            }

            function removeVendor(preset) {
                fetch('/api/presets/remove', {
                    method: 'POST',
                    headers: {'Content-Type':'application/x-www-form-urlencoded'},
                    body: 'name=' + encodeURIComponent(preset)
                }).then(function(){
                    var el = document.getElementById('sig-' + preset);
                    if (el) el.remove();
                });
            }

            function appendOUIs(ouiStr) {
                // Deprecated — kept for backwards compat, delegates to addFilterRow
                var ouis = ouiStr.split(',').map(function(s){return s.trim();}).filter(Boolean);
                ouis.forEach(function(oui){
                    addFilterRow(oui.replace(':','').toUpperCase(), true, true, 'OUI: ' + oui);
                });
            }
            
            function burnInConfig() {
                if (!confirm('PERMANENT CONFIGURATION LOCK\n\nThis will PERMANENTLY lock all settings (filters, aliases, buzzer/LED preferences).\n\nAfter activation:\n- WiFi AP and config window disabled on boot\n- Device boots directly to scanning mode\n- Unlock: hold BOOT during power-on (or erase flash + reflash)\n\nClick OK to proceed with permanent lock.')) {
                    return;
                }

                // Build filter JSON from DOM
                buildFilterList();

                // Collect current form values
                const formData = new URLSearchParams();
                const filtersJson = document.getElementById('filtersJson') ? document.getElementById('filtersJson').value : '[]';
                const buzzerEnabled = document.getElementById('buzzerEnabled') ? document.getElementById('buzzerEnabled').checked : true;
                const ledEnabled = document.getElementById('ledEnabled') ? document.getElementById('ledEnabled').checked : true;
                const apSSID = document.getElementById('ap_ssid') ? document.getElementById('ap_ssid').value : '';
                const apPassword = document.getElementById('ap_password') ? document.getElementById('ap_password').value : '';

                formData.append('filters', filtersJson);
                if (buzzerEnabled) formData.append('buzzerEnabled', 'on');
                if (ledEnabled) formData.append('ledEnabled', 'on');
                formData.append('ap_ssid', apSSID);
                formData.append('ap_password', apPassword);
                
                // User confirmed, proceed with burn-in - send current form values
                fetch('/api/lock-config', { 
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/x-www-form-urlencoded',
                    },
                    body: formData.toString()
                })
                    .then(response => response.text())
                    .then(data => {
                        // Response is HTML that shows the success page
                        document.open();
                        document.write(data);
                        document.close();
                    })
                    .catch(error => {
                        console.error('Error:', error);
                        alert('Error locking configuration. Check console.');
                    });
            }
            
            // === Filter List Management ===

            var dirtyState = false;

            function setDirty() { dirtyState = true; }
            function clearDirty() { dirtyState = false; }

            function checkDirtyBeforePreset() {
                if (dirtyState) {
                    return confirm('You have unsaved changes. Apply preset anyway? (unsaved changes will be lost)');
                }
                return true;
            }

            function formatMac(hexStr) {
                // Format hex string with colons: AA:BB:CC or AA:BB:CC:DD:EE:FF
                var s = hexStr.replace(/:/g, '').toUpperCase();
                if (s.length <= 6) {
                    return s.padStart(6, '0').replace(/(..)(..)(..)/, '$1:$2:$3');
                }
                return s.padStart(12, '0').replace(/(..)(..)(..)(..)(..)(..)/, '$1:$2:$3:$4:$5:$6');
            }

            function validateHexString(raw) {
                // Accept formats: AA:BB:CC, AABBCC, AA:BB:CC:DD:EE:FF, AABBCCDDEEFF
                var clean = raw.replace(/[:\s]/g, '').toUpperCase();
                if (!/^[0-9A-F]+$/.test(clean)) return null;
                if (clean.length !== 6 && clean.length !== 12) return null;
                return clean;
            }

            function addFilterRow(oui, ble, wifi, desc) {
                if (!checkDirtyBeforePreset()) return;

                var clean = oui.replace(/[:\s]/g, '').toUpperCase();
                if (!/^[0-9A-F]+$/.test(clean)) {
                    alert('Invalid hex value. Use XX:XX:XX or XX:XX:XX:XX:XX:XX format.');
                    return;
                }
                if (clean.length !== 6 && clean.length !== 12) {
                    alert('OUI must be 3 or 6 hex octets (6 or 12 hex chars).');
                    return;
                }
                if (!ble && !wifi) return;

                // Check for duplicate — same OUI with overlapping domains
                var list = document.getElementById('filterList');
                var rows = list.querySelectorAll('.filter-row');
                var existingIdx = -1;
                for (var i = 0; i < rows.length; i++) {
                    var r = rows[i];
                    if (r.getAttribute('data-preset') === 'true') continue;
                    if (r.getAttribute('data-oui') === clean) {
                        existingIdx = i;
                        break;
                    }
                }
                if (existingIdx >= 0) {
                    var exRow = rows[existingIdx];
                    var exBle = exRow.getAttribute('data-ble') === 'true';
                    var exWifi = exRow.getAttribute('data-wifi') === 'true';
                    if (ble && exBle && wifi && exWifi) {
                        alert(formatMac(clean) + ' is already in the list with the same domains.');
                        return;
                    }
                    // Merge: enable the new domains on the existing row
                    if (ble) exRow.setAttribute('data-ble', 'true');
                    if (wifi) exRow.setAttribute('data-wifi', 'true');
                    renderDomains(exRow);
                    setDirty();
                    return;
                }

                if (!desc) {
                    desc = clean.length === 6 ? 'OUI: ' + formatMac(clean) : 'Full MAC: ' + formatMac(clean);
                }

                var row = document.createElement('div');
                row.className = 'filter-row';
                row.setAttribute('data-oui', clean);
                row.setAttribute('data-ble', ble ? 'true' : 'false');
                row.setAttribute('data-wifi', wifi ? 'true' : 'false');
                row.setAttribute('data-desc', desc);

                row.innerHTML =
                    '<span class="device-mac">' + formatMac(clean) + '</span>' +
                    '<span class="filter-domains">' +
                        (ble ? '<span class="match-badge type-BLE" onclick="toggleBadge(this)">BLE</span>' : '') +
                        (wifi ? '<span class="match-badge type-WiFi" onclick="toggleBadge(this)">WiFi</span>' : '') +
                    '</span>' +
                    '<span class="device-filter">' + desc + '</span>' +
                    '<button type="button" class="remove-filter-btn" onclick="removeFilterRow(this)">&times;</button>';

                list.appendChild(row);
                setDirty();
                updateEmptyState();
            }

            function renderDomains(row) {
                var domains = row.querySelector('.filter-domains');
                if (!domains) return;
                var ble = row.getAttribute('data-ble') === 'true';
                var wifi = row.getAttribute('data-wifi') === 'true';
                domains.innerHTML =
                    '<span class="match-badge type-BLE' + (ble ? '' : ' disabled') + '" onclick="toggleBadge(this)">BLE</span>' +
                    '<span class="match-badge type-WiFi' + (wifi ? '' : ' disabled') + '" onclick="toggleBadge(this)">WiFi</span>';
            }

            function removeFilterRow(buttonEl) {
                var row = buttonEl.closest('.filter-row');
                if (!row) return;
                if (row.getAttribute('data-preset') === 'true') return;
                row.remove();
                setDirty();
                updateEmptyState();
            }

            function toggleBadge(badgeEl) {
                var row = badgeEl.closest('.filter-row');
                if (!row) return;
                if (row.getAttribute('data-preset') === 'true') return;

                var text = badgeEl.textContent.trim();
                if (text === 'BLE') {
                    var current = row.getAttribute('data-ble') === 'true';
                    row.setAttribute('data-ble', current ? 'false' : 'true');
                    badgeEl.classList.toggle('disabled', !current);
                } else if (text === 'WiFi') {
                    var current = row.getAttribute('data-wifi') === 'true';
                    row.setAttribute('data-wifi', current ? 'false' : 'true');
                    badgeEl.classList.toggle('disabled', !current);
                }

                setDirty();
            }

            function addFilterFromInput() {
                var input = document.getElementById('filterInput');
                var raw = input.value.trim();
                if (!raw) {
                    alert('Please enter an OUI or MAC address.');
                    return;
                }
                var clean = validateHexString(raw);
                if (!clean) {
                    alert('Invalid format. Use XX:XX:XX (OUI) or XX:XX:XX:XX:XX:XX (full MAC).');
                    return;
                }
                addFilterRow(clean, true, true, '');
                input.value = '';
                input.focus();
            }

            function buildFilterList() {
                var list = document.getElementById('filterList');
                var rows = list.querySelectorAll('.filter-row');
                var filters = [];
                for (var i = 0; i < rows.length; i++) {
                    var r = rows[i];
                    if (r.getAttribute('data-preset') === 'true') continue;
                    filters.push({
                        oui: r.getAttribute('data-oui'),
                        ble: r.getAttribute('data-ble') === 'true',
                        wifi: r.getAttribute('data-wifi') === 'true',
                        desc: r.getAttribute('data-desc') || ''
                    });
                }
                var hidden = document.getElementById('filtersJson');
                if (hidden) hidden.value = JSON.stringify(filters);
                return filters;
            }

            function renderFilterList(htmlString) {
                var list = document.getElementById('filterList');
                if (list) {
                    list.innerHTML = htmlString;
                    updateEmptyState();
                }
            }

            function showEmptyState() {
                var el = document.getElementById('emptyFilterMsg');
                if (el) el.style.display = 'block';
            }

            function hideEmptyState() {
                var el = document.getElementById('emptyFilterMsg');
                if (el) el.style.display = 'none';
            }

            function updateEmptyState() {
                var list = document.getElementById('filterList');
                if (!list) return;
                var rows = list.querySelectorAll('.filter-row');
                if (rows.length === 0) {
                    showEmptyState();
                } else {
                    hideEmptyState();
                }
            }

            // Form submit handler: serialize filter list before submit
            (function() {
                var form = document.getElementById('configForm');
                if (form) {
                    form.addEventListener('submit', function() {
                        buildFilterList();
                    });
                }
            })();

            // On page load: ensure empty state matches initial filter list
            window.addEventListener('DOMContentLoaded', function() {
                updateEmptyState();
            });
            // populate preset signature lines on load
            loadSigLines();
        </script>
        </form>
    </div>
</body>
</html>
)html";

String generateRandomOUI() {
    String oui = "";
    for (int i = 0; i < 3; i++) {
        if (i > 0) oui += ":";
        int val = random(0, 256);
        if (val < 16) oui += "0";
        oui += String(val, HEX);
    }
    oui.toLowerCase();
    return oui;
}

String generateRandomMAC() {
    String mac = "";
    for (int i = 0; i < 6; i++) {
        if (i > 0) mac += ":";
        int val = random(0, 256);
        if (val < 16) mac += "0";
        mac += String(val, HEX);
    }
    mac.toLowerCase();
    return mac;
}

// Escape HTML special characters for safe embedding in template output.
static String escapeHtmlAttr(const String& s) {
    String out = s;
    out.replace("&", "&amp;");
    out.replace("\"", "&quot;");
    out.replace("<", "&lt;");
    out.replace(">", "&gt;");
    out.replace("%", "%%");
    return out;
}

// Format a bare hex string (6-char OUI or 12-char MAC) with colons, uppercase.
static String formatHexString(const String& hex) {
    String out = "";
    for (int i = 0; i < hex.length() && i < 12; i += 2) {
        if (i > 0) out += ":";
        out += hex.substring(i, i + 2);
    }
    out.toUpperCase();
    return out;
}

String configProcessor(const String& var) {
    // NEW: unified filter list rows — replaces %OUI_VALUES% and %MAC_VALUES%
    if (var == "FILTER_LIST_ROWS") {
        String rows;

        // Group FT_MAC_PREFIX, FT_WIFI_PROBE, FT_WIFI_BEACON, FT_FULL_MAC by identifier.
        // For OUI types, first 6 chars of identifier (the OUI) is the grouping key.
        // Collect unique keys in order of first appearance.
        typedef struct { String key; String display; bool ble; bool wifi; bool fullMac; String desc; } OuiGroup;
        std::vector<OuiGroup> groups;

        for (const TargetFilter& f : targetFilters) {
            if (f.type == FT_MAC_PREFIX || f.type == FT_WIFI_PROBE || f.type == FT_WIFI_BEACON) {
                // Group key = first 6 chars of identifier (the OUI)
                String key = f.identifier.substring(0, 6);
                key.toUpperCase();

                // Find or create group
                bool found = false;
                for (auto& g : groups) {
                    if (g.key == key) {
                        found = true;
                        if (f.type == FT_MAC_PREFIX) g.ble = true;
                        if (f.type == FT_WIFI_PROBE || f.type == FT_WIFI_BEACON) g.wifi = true;
                        // Keep the longest/most descriptive description
                        if (f.description.length() > g.desc.length()) g.desc = f.description;
                        break;
                    }
                }
                if (!found) {
                    OuiGroup g;
                    g.key = key;
                    g.display = formatHexString(key);
                    g.ble = (f.type == FT_MAC_PREFIX);
                    g.wifi = (f.type == FT_WIFI_PROBE || f.type == FT_WIFI_BEACON);
                    g.fullMac = false;
                    g.desc = f.description;
                    groups.push_back(g);
                }
            } else if (f.type == FT_FULL_MAC) {
                // Full MAC — each is its own row, grouped by full 12-char identifier
                String key = f.identifier.substring(0, 12);
                key.toUpperCase();
                // Check for duplicate
                bool found = false;
                for (const auto& g : groups) {
                    if (g.key == key) { found = true; break; }
                }
                if (!found) {
                    OuiGroup g;
                    g.key = key;
                    g.display = formatHexString(key);
                    g.ble = true; // Full MAC shown as BLE by default
                    g.wifi = false;
                    g.fullMac = true;
                    g.desc = f.description;
                    groups.push_back(g);
                }
            }
            // Non-OUI types (CID, SVC, NAME, META) handled below
        }

        // Build HTML rows for grouped OUI/MAC entries
        for (const auto& g : groups) {
            String escapedDesc = escapeHtmlAttr(g.desc);
            String escapedOui = escapeHtmlAttr(g.key);
            String escapedDisplay = escapeHtmlAttr(g.display);
            String bleAttr = g.ble ? "true" : "false";
            String wifiAttr = g.wifi ? "true" : "false";

            rows += "<div class=\"filter-row\" data-oui=\"" + escapedOui + "\" data-ble=\"" + bleAttr + "\" data-wifi=\"" + wifiAttr + "\" data-desc=\"" + escapedDesc + "\">";
            rows += "<span class=\"device-mac\">" + escapedDisplay + "</span>";
            rows += "<span class=\"filter-domains\">";
            String bleClass = g.ble ? "" : " disabled";
            rows += "<span class=\"match-badge type-BLE" + bleClass + "\" onclick=\"toggleBadge(this)\">BLE</span>";
            String wifiClass = g.wifi ? "" : " disabled";
            rows += "<span class=\"match-badge type-WiFi" + wifiClass + "\" onclick=\"toggleBadge(this)\">WiFi</span>";
            rows += "</span>";
            if (!g.fullMac) {
                rows += "<button type=\"button\" class=\"remove-filter-btn\" onclick=\"removeFilterRow(this)\">&times;</button>";
            }
            rows += "</div>";
        }

        // Preset-only rows: FT_COMPANY_ID, FT_SERVICE_UUID_16, FT_NAME_SUBSTRING, FT_META_COMPOSITE
        for (const TargetFilter& f : targetFilters) {
            if (f.type != FT_COMPANY_ID && f.type != FT_SERVICE_UUID_16 &&
                f.type != FT_NAME_SUBSTRING && f.type != FT_META_COMPOSITE) continue;

            String badgeType = String(filterTypeCode(f.type));
            String escapedDesc = escapeHtmlAttr(f.description);
            String escapedId = escapeHtmlAttr(f.identifier);
            // Format identifier for display — short hex values as colon-separated
            String displayId = f.identifier;
            if (displayId.length() == 4) {
                displayId = displayId.substring(0,2) + ":" + displayId.substring(2,4);
            }
            displayId.toUpperCase();
            String escapedDisplay = escapeHtmlAttr(displayId);

            rows += "<div class=\"filter-row preset-only\" data-preset=\"true\" data-oui=\"" + escapedId + "\">";
            rows += "<span class=\"device-mac\">" + escapedDisplay + "</span>";
            rows += "<span class=\"filter-domains\"><span class=\"match-badge type-" + badgeType + "\">" + badgeType + "</span></span>";
            rows += "<span class=\"device-filter\">" + escapedDesc + "</span>";
            rows += "</div>";
        }

        return rows;
    }

    // Legacy placeholders — kept for backward compat but render empty
    if (var == "OUI_VALUES") return "";
    if (var == "MAC_VALUES") return "";

    if (var == "BUZZER_CHECKED") return buzzerEnabled ? "checked" : "";
    if (var == "LED_CHECKED") return ledEnabled ? "checked" : "";
    if (var == "AP_SSID") return AP_SSID;
    if (var == "AP_PASSWORD") return AP_PASSWORD;
    if (var == "ASCII_ART") return "";
    if (var == "MQTT_EN") return mqttCfg.enabled ? "checked" : "";
    if (var == "MQ_SS") return mqttCfg.sta_ssid;
    if (var == "MQ_SP") return mqttCfg.sta_pass;
    if (var == "MQ_BK") return mqttCfg.broker;
    if (var == "MQ_PT") return String(mqttCfg.port);
    if (var == "MQ_US") return mqttCfg.user;
    if (var == "MQ_PW") return mqttCfg.pass;
    if (var == "MQ_ID") return mqttCfg.device_id;
    if (var == "MQ_TP") return mqttCfg.topic;
    return String();
}

// Shared: parse the 'filters' JSON form field and rebuild targetFilters.
// Clears user-managed filter types (FT_MAC_PREFIX, FT_FULL_MAC,
// FT_WIFI_PROBE, FT_WIFI_BEACON) first; preserves preset-only types.
// Returns the number of new filter entries added.
static int parseFiltersFromJSON(AsyncWebServerRequest *request) {
    // Clear user-managed filter types
    targetFilters.erase(
        std::remove_if(targetFilters.begin(), targetFilters.end(),
            [](const TargetFilter& f) {
                return f.type == FT_MAC_PREFIX || f.type == FT_FULL_MAC ||
                       f.type == FT_WIFI_PROBE || f.type == FT_WIFI_BEACON;
            }),
        targetFilters.end());

    if (!request->hasParam("filters", true)) {
        return 0;
    }
    String filtersJson = request->getParam("filters", true)->value();
    filtersJson.trim();
    if (filtersJson.length() == 0) return 0;

    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, filtersJson);
    if (err.code() != DeserializationError::Ok) {
        if (isSerialConnected()) Serial.print("filters JSON parse error: ");
        if (isSerialConnected()) Serial.println(err.c_str());
        return 0;
    }

    int added = 0;
    if (!doc.is<JsonArray>()) return 0;
    for (JsonObject entry : doc.as<JsonArray>()) {
        const char* ouiRaw = entry["oui"];
        if (ouiRaw == NULL || strlen(ouiRaw) == 0) continue;

        // Normalize OUI to uppercase bare hex (6 or 12 chars), strip colons
        String ouiStr(ouiRaw);
        ouiStr.toUpperCase();
        ouiStr.replace(":", "");
        ouiStr.replace("-", "");

        bool ble = entry["ble"] | false;
        bool wifi = entry["wifi"] | false;
        const char* descRaw = entry["desc"];
        String desc = descRaw ? String(descRaw) : "";

        // Detect full MAC (12 hex chars) vs OUI (6 hex chars)
        bool isFullMac = (ouiStr.length() == 12);

        if (isFullMac) {
            // Full MAC entry — treat as FT_FULL_MAC, BLE by default
            TargetFilter f;
            f.identifier = ouiStr;
            f.description = desc.isEmpty() ? "MAC: " + ouiStr : desc;
            f.isFullMAC = true;
            f.type = FT_FULL_MAC;
            targetFilters.push_back(f);
            added++;
        } else {
            // OUI entry — expand based on ble/wifi flags
            String normalizedOUI = ouiStr;
            normalizedOUI.toUpperCase();

            if (ble) {
                TargetFilter f;
                f.identifier = normalizedOUI;
                // Format with colons for description
                String formatted = normalizedOUI.substring(0,2) + ":" + normalizedOUI.substring(2,4) + ":" + normalizedOUI.substring(4,6);
                f.description = desc.isEmpty() ? "OUI: " + formatted : desc;
                f.isFullMAC = false;
                f.type = FT_MAC_PREFIX;
                targetFilters.push_back(f);
                added++;
            }
            if (wifi) {
                TargetFilter f;
                f.identifier = normalizedOUI;
                String formatted = normalizedOUI.substring(0,2) + ":" + normalizedOUI.substring(2,4) + ":" + normalizedOUI.substring(4,6);
                f.description = desc.isEmpty() ? "OUI: " + formatted + " (WiFi)" : desc;
                f.isFullMAC = false;
                f.type = FT_WIFI_PROBE;
                targetFilters.push_back(f);
                added++;
            }
        }
    }
    rebuildWifiOuiTable();
    return added;
}

// Android captive portal detection (expects 204 response, we send redirect instead)
void handleGenerate204(AsyncWebServerRequest *request) {
    request->redirect("http://192.168.4.1/");
}

void handleCaptiveDetect(AsyncWebServerRequest *request) {
    request->send(200, "text/html", "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='0;url=http://192.168.4.1/'></head><body><a href='http://192.168.4.1/'>Click here to configure OUI SPY detector</a></body></html>");
}

// ================================
// WiFi and Web Server Functions
// ================================
void startConfigMode() {
    currentMode = CONFIG_MODE;
    
    Serial.println("\n=== STARTING CONFIG MODE ===");
    Serial.println("SSID: " + AP_SSID);
    Serial.println("Password: " + AP_PASSWORD);
    Serial.println("Initializing WiFi AP...");
    
    // Ensure WiFi is off first
    WiFi.mode(WIFI_OFF);
    delay(1000);
    
    // Start WiFi AP
    Serial.println("Setting WiFi mode to AP...");
    WiFi.mode(WIFI_AP);
    delay(500);
    
    Serial.println("Creating access point...");
    bool apStarted = WiFi.softAP(AP_SSID.c_str(), AP_PASSWORD.c_str());
    
    if (apStarted) {
        Serial.println("✓ Access Point created successfully!");
    } else {
        Serial.println("✗ Failed to create Access Point!");
        return;
    }
    
    delay(2000); // Give AP time to fully initialize
    
    IPAddress IP = WiFi.softAPIP();
    Serial.println("AP IP address: " + IP.toString());
    Serial.println("Config portal: http://" + IP.toString());
    
    // Start DNS server for captive portal - redirect all DNS queries to our IP
    dnsServer.start(DNS_PORT, "*", captivePortalIP);
    Serial.println("DNS server started (captive portal active)");
    Serial.println("==============================\n");
    
    // Setup web server routes
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {

        request->send_P(200, "text/html", CONFIG_HTML, configProcessor);
    });
    
    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {

        if (isSerialConnected()) {
            Serial.println("\n=== WEB CONFIG SUBMISSION ===");
        }

        // Parse filters from JSON array (new filter UI format)
        int filterCount = parseFiltersFromJSON(request);

        // Process buzzer and LED toggles
        // (only set to true if explicitly provided — otherwise leave at saved state)
        if (request->hasParam("buzzerEnabled", true)) {
            buzzerEnabled = true;
        }
        if (request->hasParam("ledEnabled", true)) {
            ledEnabled = true;
        }
        
        // Process WiFi credentials
        if (request->hasParam("ap_ssid", true)) {
            String newSSID = request->getParam("ap_ssid", true)->value();
            newSSID.trim();
            if (newSSID.length() > 0 && newSSID.length() <= 32) {
                AP_SSID = newSSID;
            }
        }
        
        if (request->hasParam("ap_password", true)) {
            String newPassword = request->getParam("ap_password", true)->value();
            newPassword.trim();
            // Allow empty password for open network, or 8-63 chars
            if (newPassword.length() == 0 || (newPassword.length() >= 8 && newPassword.length() <= 63)) {
                AP_PASSWORD = newPassword;
            }
        }
        
        // Save WiFi credentials
        saveWiFiCredentials();

        // Process MQTT
        mqttCfg.enabled = request->hasParam("mqtt_en", true);
        if (request->hasParam("mq_id", true)) strlcpy(mqttCfg.device_id, request->getParam("mq_id", true)->value().c_str(), sizeof(mqttCfg.device_id));
        if (request->hasParam("mq_ss", true)) strlcpy(mqttCfg.sta_ssid, request->getParam("mq_ss", true)->value().c_str(), sizeof(mqttCfg.sta_ssid));
        if (request->hasParam("mq_sp", true)) strlcpy(mqttCfg.sta_pass, request->getParam("mq_sp", true)->value().c_str(), sizeof(mqttCfg.sta_pass));
        if (request->hasParam("mq_bk", true)) strlcpy(mqttCfg.broker, request->getParam("mq_bk", true)->value().c_str(), sizeof(mqttCfg.broker));
        if (request->hasParam("mq_pt", true)) mqttCfg.port = request->getParam("mq_pt", true)->value().toInt();
        if (request->hasParam("mq_us", true)) strlcpy(mqttCfg.user, request->getParam("mq_us", true)->value().c_str(), sizeof(mqttCfg.user));
        if (request->hasParam("mq_pw", true)) strlcpy(mqttCfg.pass, request->getParam("mq_pw", true)->value().c_str(), sizeof(mqttCfg.pass));
        if (request->hasParam("mq_tp", true)) strlcpy(mqttCfg.topic, request->getParam("mq_tp", true)->value().c_str(), sizeof(mqttCfg.topic));
        if (mqttCfg.device_id[0] == 0) strlcpy(mqttCfg.device_id, "ouispy", sizeof(mqttCfg.device_id));
        if (mqttCfg.port == 0) mqttCfg.port = 1883;
        if (mqttCfg.topic[0] == 0) snprintf(mqttCfg.topic, sizeof(mqttCfg.topic), "%s/detection", mqttCfg.device_id);
        mqtt_saveConfig();

        if (isSerialConnected()) {
            Serial.println("Buzzer enabled: " + String(buzzerEnabled ? "Yes" : "No"));
            Serial.println("LED enabled: " + String(ledEnabled ? "Yes" : "No"));
            Serial.println("WiFi SSID: " + AP_SSID);
            Serial.println("WiFi Password: " + String(AP_PASSWORD.length() > 0 ? "********" : "(Open Network)"));
        }
        
        if (targetFilters.size() > 0) {
            saveConfiguration();
            
            if (isSerialConnected()) {
                Serial.println("Saved " + String(targetFilters.size()) + " filters:");
                for (const TargetFilter& filter : targetFilters) {
                    String type = filter.isFullMAC ? "Full MAC" : "OUI";
                    Serial.println("  - " + filter.identifier + " (" + type + ")");
                }
            }
            
            String responseHTML = R"html(
<!DOCTYPE html>
<html>
<head>
    <title>Configuration Saved</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { 
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
            margin: 0; 
            padding: 20px;
            background: #1a1a1a; 
            color: #e0e0e0;
            text-align: center; 
        }
        .container { 
            max-width: 600px; 
            margin: 0 auto; 
            background: #2d2d2d; 
            padding: 40px; 
            border-radius: 12px; 
            box-shadow: 0 4px 20px rgba(0,0,0,0.3); 
        }
        h1 { 
            color: #ffffff; 
            margin-bottom: 30px; 
            font-weight: 300;
        }
        .success { 
            background: #1a4a3a; 
            color: #4ade80; 
            border: 1px solid #166534; 
            padding: 20px; 
            border-radius: 8px; 
            margin: 30px 0; 
        }
        p { 
            line-height: 1.6; 
            margin: 15px 0;
        }
    </style>
    <script>
        setTimeout(function() {
            document.getElementById('countdown').innerHTML = 'Switching to scanning mode now...';
        }, 5000);
    </script>
</head>
<body>
    <div class="container">
        <h1>Configuration Saved</h1>
        <div class="success">
            <p><strong>Saved )html" + String(targetFilters.size()) + R"html( filters successfully!</strong></p>
            <p id="countdown">Switching to scanning mode in 5 seconds...</p>
        </div>
        <p>The device will now start scanning for your configured devices.</p>
        <p>When a match is found, you'll hear the buzzer alerts!</p>
    </div>
</body>
</html>
)html";
            
            request->send(200, "text/html", responseHTML);
            
            // Schedule mode switch for 5 seconds from now
            modeSwitchScheduled = millis() + 5000;
            
            if (isSerialConnected()) {
                Serial.println("Mode switch scheduled for 5 seconds from now");
                Serial.println("==============================\n");
            }
        } else {
            request->send(400, "text/html", "<h1>Error: No valid filters provided</h1>");
        }
    });
    
    server.on("/clear", HTTP_POST, [](AsyncWebServerRequest *request) {

        
        // Clear all filters
        targetFilters.clear();
        saveConfiguration();
        
        if (isSerialConnected()) {
            Serial.println("All filters cleared via web interface");
        }
        
        request->send(200, "text/plain", "Filters cleared successfully");
    });
    
    // Device reset - completely wipe saved config and restart
    server.on("/device-reset", HTTP_POST, [](AsyncWebServerRequest *request) {

        
        if (isSerialConnected()) {
            Serial.println("DEVICE RESET - Request received, scheduling reset...");
        }
        
        request->send(200, "text/html", 
            "<html><body style='background:#1a1a1a;color:#e0e0e0;font-family:Arial;text-align:center;padding:50px;'>"
            "<h1>Device Reset Complete</h1>"
            "<p>Device restarting in 3 seconds...</p>"
            "<script>setTimeout(function(){window.location.href='/';}, 5000);</script>"
            "</body></html>");
        
        // Just schedule device reset - do all clearing in main loop
        deviceResetScheduled = millis() + 3000;
    });
    
    // API endpoint to get detected devices
    server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest *request) {

        
        String json = "{\"devices\":[";
        
        unsigned long currentTime = millis();
        
        for (size_t i = 0; i < devices.size(); i++) {
            if (i > 0) json += ",";
            
            String alias = getDeviceAlias(devices[i].macAddress);
            String filterDesc = devices[i].filterDescription;
            if (filterDesc.length() == 0 && devices[i].matchedFilter) {
                filterDesc = String(devices[i].matchedFilter);
            }
            
            // Calculate time since last seen
            unsigned long timeSince = (currentTime >= devices[i].lastSeen) ? 
                                     (currentTime - devices[i].lastSeen) : 0;
            
            json += "{";
            json += "\"mac\":\"" + devices[i].macAddress + "\",";
            json += "\"rssi\":" + String(devices[i].rssi) + ",";
            json += "\"filter\":\"" + filterDesc + "\",";
            json += "\"type\":\"" + String(filterTypeCode(devices[i].matchedType)) + "\",";
            json += "\"match\":\"" + devices[i].matchedIdentifier + "\",";
            json += "\"alias\":\"" + alias + "\",";
            json += "\"lastSeen\":" + String(devices[i].lastSeen) + ",";
            json += "\"timeSince\":" + String(timeSince);
            json += "}";
        }
        
        json += "],";
        json += "\"currentTime\":" + String(currentTime);
        json += "}";
        
        request->send(200, "application/json", json);
    });
    
    // API endpoint to save device alias
    server.on("/api/alias", HTTP_POST, [](AsyncWebServerRequest *request) {

        
        if (request->hasParam("mac", true) && request->hasParam("alias", true)) {
            String mac = request->getParam("mac", true)->value();
            String alias = request->getParam("alias", true)->value();
            
            setDeviceAlias(mac, alias);
            saveDeviceAliases();
            
            if (isSerialConnected()) {
                if (alias.length() > 0) {
                    Serial.println("Alias saved: " + mac + " -> \"" + alias + "\"");
                } else {
                    Serial.println("Alias removed: " + mac);
                }
            }
            
            request->send(200, "application/json", "{\"success\":true}");
        } else {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing parameters\"}");
        }
    });
    
    // API endpoint to clear device history
    server.on("/api/clear-devices", HTTP_POST, [](AsyncWebServerRequest *request) {

        
        clearDetectedDevices();
        
        if (isSerialConnected()) {
            Serial.println("Device history cleared via web interface");
        }
        
        request->send(200, "application/json", "{\"success\":true}");
    });
    
    // API endpoint to lock/burn-in configuration
    server.on("/api/lock-config", HTTP_POST, [](AsyncWebServerRequest *request) {

        if (isSerialConnected()) {
            Serial.println("======================================");
            Serial.println("CONFIGURATION LOCK REQUESTED");
            Serial.println("Saving current form values before locking...");
            Serial.println("======================================");
        }

        // Parse filters from JSON array (same shared logic as /save)
        parseFiltersFromJSON(request);

        // Process buzzer and LED toggles
        // (only set to true if explicitly provided — otherwise leave at saved state)
        if (request->hasParam("buzzerEnabled", true)) {
            buzzerEnabled = true;
        }
        if (request->hasParam("ledEnabled", true)) {
            ledEnabled = true;
        }

        // Process WiFi credentials
        if (request->hasParam("ap_ssid", true)) {
            String newSSID = request->getParam("ap_ssid", true)->value();
            newSSID.trim();
            if (newSSID.length() > 0 && newSSID.length() <= 32) {
                AP_SSID = newSSID;
            }
        }

        if (request->hasParam("ap_password", true)) {
            String newPassword = request->getParam("ap_password", true)->value();
            newPassword.trim();
            // Allow empty password for open network, or 8-63 chars
            if (newPassword.length() == 0 || (newPassword.length() >= 8 && newPassword.length() <= 63)) {
                AP_PASSWORD = newPassword;
            }
        }
        
        // Save WiFi credentials
        saveWiFiCredentials();
        
        // Save configuration (even if empty - that's what user wants)
        saveConfiguration();
        
        if (isSerialConnected()) {
            Serial.println("Buzzer enabled: " + String(buzzerEnabled ? "Yes" : "No"));
            Serial.println("LED enabled: " + String(ledEnabled ? "Yes" : "No"));
            Serial.println("WiFi SSID: " + AP_SSID);
            Serial.println("WiFi Password: " + String(AP_PASSWORD.length() > 0 ? "********" : "(Open Network)"));
            Serial.println("Saved " + String(targetFilters.size()) + " filters before locking:");
            for (const TargetFilter& filter : targetFilters) {
                String type = filter.isFullMAC ? "Full MAC" : "OUI";
                Serial.println("  - " + filter.identifier + " (" + type + ")");
            }
        }
        
        // Set the lock flag
        preferences.begin("ouispy", false);
        preferences.putBool("configLocked", true);
        preferences.end();
        
        if (isSerialConnected()) {
            Serial.println("Configuration locked successfully!");
            Serial.println("Device will skip config mode on next boot");
            Serial.println("Unlock: hold BOOT at power-on, or erase flash + reflash");
        }
        
        String responseHTML = R"html(
<!DOCTYPE html>
<html>
<head>
    <title>Configuration Locked</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { 
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
            margin: 0; 
            padding: 20px;
            background: linear-gradient(135deg, #1a1a1a 0%, #0a0a0a 100%); 
            color: #e0e0e0;
            text-align: center;
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
        }
        .container { 
            max-width: 750px; 
            margin: 0 auto; 
            background: linear-gradient(135deg, #2d2d2d 0%, #1a1a1a 100%);
            padding: 50px; 
            border-radius: 16px; 
            box-shadow: 0 8px 32px rgba(0,0,0,0.5); 
            border: 2px solid rgba(139, 0, 0, 0.3);
        }
        h1 { 
            color: #ff6b6b; 
            margin-bottom: 30px;
            font-size: 32px;
            font-weight: 600;
            letter-spacing: 1px;
            text-transform: uppercase;
        }
        .warning { 
            background: linear-gradient(135deg, #1a0a0a 0%, #2d0a0a 100%);
            color: #ffcccc; 
            border: 2px solid #8b0000; 
            padding: 25px; 
            border-radius: 10px; 
            margin: 25px 0; 
            font-weight: 500;
            box-shadow: 0 4px 15px rgba(139, 0, 0, 0.3);
        }
        .info {
            background: linear-gradient(135deg, #0a1a0a 0%, #0a2d0a 100%);
            color: #ccffcc; 
            border: 1px solid #166534; 
            padding: 25px; 
            border-radius: 10px; 
            margin: 25px 0;
            box-shadow: 0 2px 10px rgba(22, 101, 52, 0.2);
        }
        p { 
            line-height: 1.8; 
            margin: 15px 0; 
            font-size: 15px;
        }
        .status-item {
            text-align: left;
            padding: 10px 0;
            border-bottom: 1px solid rgba(255, 255, 255, 0.05);
        }
        .status-item:last-child {
            border-bottom: none;
        }
        .countdown {
            font-size: 16px;
            color: #888888;
            margin-top: 30px;
            font-style: italic;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>Configuration Locked</h1>
        <div class="warning">
            <p style="font-size: 18px; margin-top: 0;"><strong>CONFIGURATION HAS BEEN PERMANENTLY LOCKED</strong></p>
            <p style="margin-bottom: 0;">20-second configuration window has been disabled for all future boots</p>
        </div>
        <div class="info">
            <p style="font-weight: 600; margin-top: 0; color: #4ade80; font-size: 16px; text-transform: uppercase; letter-spacing: 0.5px;">Active Configuration:</p>
            <div class="status-item">Device transitions directly to scanning mode on boot</div>
            <div class="status-item">Current OUI/MAC filters permanently saved to memory</div>
            <div class="status-item">WiFi access point disabled</div>
            <div class="status-item">Web configuration interface disabled</div>
            <div class="status-item">Reduced boot time (approximately 2 seconds)</div>
            <div class="status-item">Optimized power consumption</div>
        </div>
        <div class="warning">
            <p style="font-weight: 600; margin-top: 0; font-size: 16px; text-transform: uppercase;">Unlock Procedure:</p>
            <p style="margin-bottom: 0;">To restore configuration access: power-cycle the device holding the BOOT button for 1.5s. Erasing flash also works.</p>
        </div>
        <p class="countdown">Device will restart and begin scanning in 3 seconds...</p>
        <script>
            setTimeout(function() {
                window.location.href = 'about:blank';
            }, 3000);
        </script>
    </div>
</body>
</html>
)html";
        
        request->send(200, "text/html", responseHTML);
        
        // Schedule normal restart after 3 seconds (NOT factory reset)
        normalRestartScheduled = millis() + 3000;
    });

    // One-click add all known signatures for a device family.
    // POST body/query: name=axon | meta
    server.on("/api/presets/apply", HTTP_POST, [](AsyncWebServerRequest *request) {


        String presetName;
        if (request->hasParam("name", true))       presetName = request->getParam("name", true)->value();
        else if (request->hasParam("name", false)) presetName = request->getParam("name", false)->value();
        presetName.toLowerCase();

        int added = 0;
        String label;
        if (presetName == "axon") {
            label = "Axon body cam";
            added = applyPreset(PRESET_AXON, PRESET_AXON_COUNT, "Axon body cam");
        } else if (presetName == "meta") {
            label = "Meta glasses";
            added = applyPreset(PRESET_META, PRESET_META_COUNT, "Meta glasses");
        } else if (presetName == "ipro") {
            label = "I-PRO";
            added = applyPreset(PRESET_IPRO, PRESET_IPRO_COUNT, "I-PRO");
        } else {
            request->send(400, "application/json",
                "{\"ok\":false,\"error\":\"unknown preset\"}");
            return;
        }

        // Filter set changed — refresh the promiscuous fast-path OUI table.
        rebuildWifiOuiTable();

        String body = "{\"ok\":true,\"preset\":\"" + presetName + "\",\"label\":\"" + label +
                      "\",\"added\":" + String(added) +
                      ",\"total_filters\":" + String(targetFilters.size()) + "}";
        request->send(200, "application/json", body);

        if (isSerialConnected()) {
            Serial.printf("Preset %s applied — %d new filters (total: %u)\n",
                          presetName.c_str(), added, (unsigned)targetFilters.size());
        }
    });

    server.on("/api/presets/remove", HTTP_POST, [](AsyncWebServerRequest *request) {

        String n;
        if (request->hasParam("name", true))       n = request->getParam("name", true)->value();
        else if (request->hasParam("name", false)) n = request->getParam("name", false)->value();
        n.toLowerCase();

        int removed = 0;
        if (n == "axon")           removed = removePreset(PRESET_AXON, PRESET_AXON_COUNT);
        else if (n == "meta")      removed = removePreset(PRESET_META, PRESET_META_COUNT);
        else if (n == "ipro")      removed = removePreset(PRESET_IPRO, PRESET_IPRO_COUNT);
        else { request->send(400, "application/json", "{\"ok\":false,\"error\":\"unknown preset\"}"); return; }

        // Filter set changed — refresh the promiscuous fast-path OUI table.
        rebuildWifiOuiTable();

        request->send(200, "application/json",
            "{\"ok\":true,\"removed\":" + String(removed) +
            ",\"total_filters\":" + String(targetFilters.size()) + "}");
    });

    server.on("/api/presets/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        String body = "{\"axon\":";
        body += presetInstalled(PRESET_AXON, PRESET_AXON_COUNT) ? "true" : "false";
        body += ",\"meta\":";
        body += presetInstalled(PRESET_META, PRESET_META_COUNT) ? "true" : "false";
        body += ",\"ipro\":";
        body += presetInstalled(PRESET_IPRO, PRESET_IPRO_COUNT) ? "true" : "false";
        body += "}";
        request->send(200, "application/json", body);
    });

    // Captive portal detection routes:
    server.on("/generate_204", HTTP_GET, handleGenerate204);
    server.on("/gen_204", HTTP_GET, handleGenerate204);
    server.on("/connecttest.txt", HTTP_GET, handleCaptiveDetect);
    server.on("/ncsi.txt", HTTP_GET, handleCaptiveDetect);
    server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveDetect);
    server.on("/library/test/success.html", HTTP_GET, handleCaptiveDetect);
    server.on("/canonical.html", HTTP_GET, handleCaptiveDetect);
    server.on("/success.txt", HTTP_GET, handleCaptiveDetect);
    
    server.begin();
    
    if (isSerialConnected()) {
        Serial.println("Web server started!");
    }
}

// ================================
void startScanningMode() {
    currentMode = SCANNING_MODE;

    // One-time radio/BLE/WiFi initialization (idempotent)
    scan_init();

    // Determine which domains are needed
    bool hasWifiFilters = false;
    bool hasBleFilters = false;
    for (const TargetFilter& f : targetFilters) {
        if (isWifiDomain(f.type)) hasWifiFilters = true;
        else hasBleFilters = true;
    }

    if (isSerialConnected()) {
        Serial.println("\n=== STARTING SCANNING MODE ===");
        Serial.println("Configured Filters:");
        for (const TargetFilter& filter : targetFilters) {
            String type = String(filterTypeCode(filter.type));
            Serial.println("- " + filter.identifier + " (" + type + "): " + filter.description);
        }
        Serial.println("==============================\n");
    }

    // Ready to scan - ascending beeps (no interference possible)
    delay(500);
    ascendingBeeps();

    // 2-second pause after ready signal
    delay(2000);

    // Transition to the appropriate initial phase
    g_phaseStartTime = millis();
    g_lastHopTime = millis();
    if (hasWifiFilters) {
        scan_to_wifi_sweep();
    } else if (hasBleFilters) {
        scan_to_ble_scan();
    }
}



// ================================
// Setup Function
// ================================
void setup() {
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);   // runtime BOOT-hold escape
    delay(2000);
    
    // Initialize Serial first
    Serial.begin(115200);
    delay(1000);
    
    // Print ASCII art banner
    Serial.println("\n\n");
    Serial.println("        _________        .__                       .__    __________               .__              ");
    Serial.println("        \\_   ___ \\  ____ |  |   ____   ____   ____ |  |   \\______   \\_____    ____ |__| ____        ");
    Serial.println("        /    \\  \\/ /  _ \\|  |  /  _ \\ /    \\_/ __ \\|  |    |     ___/\\__  \\  /    \\|  |/ ___\\       ");
    Serial.println("        \\     \\___(  <_> )  |_(  <_> )   |  \\  ___/|  |__  |    |     / __ \\|   |  \\  \\  \\___       ");
    Serial.println("         \\______  /\\____/|____/\\____/|___|  /\\___  >____/  |____|    (____  /___|  /__/\\___  >      ");
    Serial.println("                \\/                        \\/     \\/                       \\/     \\/        \\/       ");
    Serial.println("             .__                                     .___      __                 __                ");
    Serial.println("  ____  __ __|__|           ____________ ___.__.   __| _/_____/  |_  ____   _____/  |_  ___________ ");
    Serial.println(" /  _ \\|  |  \\  |  ______  /  ___/\\____ <   |  |  / __ |/ __ \\   __\\/ __ \\_/ ___\\   __\\/  _ \\_  __ \\");
    Serial.println("(  <_> )  |  /  | /_____/  \\___ \\ |  |_> >___  | / /_/ \\  ___/|  | \\  ___/\\  \\___|  | (  <_> )  | \\/");
    Serial.println(" \\____/|____/|__|         /____  >|   __// ____| \\____ |\\___  >__|  \\___  >\\___  >__|  \\____/|__|   ");
    Serial.println("                               \\/ |__|   \\/           \\/    \\/          \\/     \\/                   ");
    Serial.println("\n");
    
    // Randomize MAC address on each boot
    uint8_t newMAC[6];
    WiFi.macAddress(newMAC);
    
    Serial.print("Original MAC: ");
    for (int i = 0; i < 6; i++) {
        if (newMAC[i] < 16) Serial.print("0");
        Serial.print(newMAC[i], HEX);
        if (i < 5) Serial.print(":");
    }
    Serial.println();
    
    // Load MQTT config early so we know whether to skip MAC randomization
    mqtt_loadConfig();

    // Skip MAC randomization when MQTT is enabled (need stable IP for broker)
    if (!mqttCfg.enabled) {
        // STEALTH MODE: Randomize ALL 6 bytes for maximum anonymity
        randomSeed(analogRead(0) + micros());
        for (int i = 0; i < 6; i++) {
            newMAC[i] = random(0, 256);
        }
        newMAC[0] |= 0x02; // Set locally administered bit
        newMAC[0] &= 0xFE; // Clear multicast bit
        WiFi.mode(WIFI_STA);
        esp_wifi_set_mac(WIFI_IF_STA, newMAC);
    } else {
        Serial.println("MQTT enabled - keeping original MAC for stable DHCP");
    }
    
    Serial.print("Randomized MAC: ");
    for (int i = 0; i < 6; i++) {
        if (newMAC[i] < 16) Serial.print("0");
        Serial.print(newMAC[i], HEX);
        if (i < 5) Serial.print(":");
    }
    Serial.println();
    
    // Silence ESP-IDF logs
    esp_log_level_set("*", ESP_LOG_NONE);
    
    initializeBuzzer();
    
    // The boot melody replaces the old single test beep
    delay(500);
    playBoot();
    if (isSerialConnected()) Serial.println("[BOOT] Boot melody done");
    
    initializeNeoPixel();
    
    // Test NeoPixel
    setNeoPixelColor(255, 0, 255); // Bright pink
    delay(1000);
    setNeoPixelColor(128, 0, 255); // Purple
    delay(1000);
    
    // Check for factory reset flag first
    preferences.begin("ouispy", true); // read-only
    bool factoryReset = preferences.getBool("factoryReset", false);
    preferences.end();
    
    if (factoryReset) {
        Serial.println("FACTORY RESET FLAG DETECTED - Clearing all data...");
        
        // Clear the factory reset flag and all data
        preferences.begin("ouispy", false);
        preferences.clear(); // Wipe everything
        preferences.end();
        
        // Clear in-memory data
        targetFilters.clear();
        deviceAliases.clear();
        devices.clear();
        
        Serial.println("Factory reset complete - starting with clean state");
    } else {
        // Load configuration from NVS
        loadConfiguration();
        rebuildWifiOuiTable();
        loadWiFiCredentials();
        mqtt_loadConfig();
        loadDeviceAliases();
    }
    
    // Check if configuration is locked/burned in
    preferences.begin("ouispy", true);
    bool configLocked = preferences.getBool("configLocked", false);
    preferences.end();

    // BOOT-button escape hatch. Burn-in is otherwise irreversible: config
    // mode never comes back, so there is no way to reach the dashboard and
    // no way to undo it short of erasing flash. Holding BOOT (GPIO0) for
    // 1.5s during power-on clears the lock. Beeps while counting so the
    // hold is obviously registering; releasing early aborts.
    if (configLocked) {
        pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
        delay(50);
        if (digitalRead(0) == LOW) {
            Serial.println("BOOT held - keep holding 1.5s to clear config lock...");
            unsigned long t0 = millis();
            bool held = true;
            while (millis() - t0 < 1500) {
                if (digitalRead(0) == HIGH) { held = false; break; }
                if ((millis() - t0) % 300 < 40) {
                    ledcSetup(0, 2000, 8);
                    ledcAttachPin(BUZZER_PIN, 0);
                    ledcWrite(0, 90);
                    delay(30);
                    ledcWrite(0, 0);
                }
                delay(10);
            }
            if (held) {
                preferences.begin("ouispy", false);
                preferences.remove("configLocked");
                preferences.end();
                configLocked = false;
                Serial.println("*** CONFIG LOCK CLEARED - entering config mode ***");
                for (int i = 0; i < 3; i++) {   // triple beep = unlocked
                    ledcSetup(0, 3000, 8);
                    ledcAttachPin(BUZZER_PIN, 0);
                    ledcWrite(0, 110); delay(80);
                    ledcWrite(0, 0);   delay(60);
                }
            } else {
                Serial.println("BOOT released early - lock unchanged");
            }
        }
    }
    
    if (configLocked) {
        Serial.println("======================================");
        Serial.println("CONFIGURATION LOCKED (BURNED IN)");
        Serial.println("Skipping config mode - going straight to scanning");
        Serial.println("To unlock: hold BOOT during power-on (or erase flash)");
        Serial.println("======================================");
        
        // Start scanning immediately
        startScanningMode();
    } else {
        // Start in configuration mode
        Serial.println("Starting configuration mode...");
        startConfigMode();
    }
}

// ================================
// Loop Function
// ================================
void loop() {
    checkBootButtonLoop();   // BOOT hold -> clear lock, back to config mode
    static unsigned long lastScanTime = 0;
    static unsigned long lastCleanupTime = 0;
    static unsigned long lastStatusTime = 0;
    unsigned long currentMillis = millis();
    
    if (currentMode == CONFIG_MODE) {
        // Check for scheduled normal restart (from burn-in config)
        if (normalRestartScheduled > 0 && currentMillis >= normalRestartScheduled) {
            if (isSerialConnected()) {
                Serial.println("Scheduled normal restart - rebooting with locked configuration...");
            }
            
            delay(500); // Give time for any pending operations
            ESP.restart(); // Simple restart - settings preserved
        }
        
        // Check for scheduled device reset (from web device reset)
        if (deviceResetScheduled > 0 && currentMillis >= deviceResetScheduled) {
            if (isSerialConnected()) {
                Serial.println("Scheduled device reset - setting factory reset flag and restarting...");
            }
            
            // Just set a factory reset flag - much safer than complex NVS operations
            preferences.begin("ouispy", false);
            preferences.putBool("factoryReset", true);
            preferences.end();
            
            delay(500); // Give time for NVS write
            ESP.restart(); // Restart - clearing will happen safely on boot
        }
        
        // Check for scheduled mode switch (from web config save)
        if (modeSwitchScheduled > 0 && currentMillis >= modeSwitchScheduled) {
            if (isSerialConnected()) {
                Serial.println("Scheduled mode switch - switching to scanning mode");
            }
            modeSwitchScheduled = 0; // Reset
            startScanningMode();
            return;
        }
        
        // Stay in config mode until the web UI submits a config
        
        // Process DNS requests for captive portal
        dnsServer.processNextRequest();
        
        // Handle web server
        delay(100);
        return;
    }
    
    // Scanning mode loop — time-sliced WiFi/BLE with ring-buffer drain
    if (currentMode == SCANNING_MODE) {
        while (true) {
            unsigned long currentMillis = millis();
            bool hasWifiFilters = false;
        bool hasBleFilters = false;
        for (const TargetFilter& f : targetFilters) {
            if (isWifiDomain(f.type)) hasWifiFilters = true;
            else hasBleFilters = true;
        }

        if (!hasWifiFilters && !hasBleFilters) {
            scan_exit();
            delay(100);
            return;
        }

        unsigned long phaseDur = (g_radioPhase == PHASE_WIFI_SWEEP) ? WIFI_SWEEP_MS : BLE_SCAN_MS;

        // Phase transition on timeout
        if (currentMillis - g_phaseStartTime >= phaseDur) {
            if (hasWifiFilters && hasBleFilters) {
                if (g_radioPhase == PHASE_WIFI_SWEEP) {
                    scan_to_ble_scan();
                } else {
                    scan_to_wifi_sweep();
                }
            }
            g_phaseStartTime = currentMillis;
        }

        // Channel hop while in WiFi sweep phase
        if (g_radioPhase == PHASE_WIFI_SWEEP && hasWifiFilters) {
            if (currentMillis - g_lastHopTime >= WIFI_HOP_MS) {
                scan_wifi_hop_channel();
                g_lastHopTime = currentMillis;
            }
        }

        // Drain the ring buffer — replaces old newMatchFound polling
        if (detQueueCount > 0) {
            uint8_t tail = detQueueTail;
            DetectionEntry* e = &detQueue[tail];
            String alias = getDeviceAlias(String(e->mac));
            String payload = "{\"mac\":\"" + String(e->mac) + "\",\"alias\":\"" + alias +
                             "\",\"rssi\":" + String(e->rssi) + ",\"type\":\"" +
                             filterTypeCode(e->matchedType) + "\",\"match\":\"" +
                             e->identifier + "\",\"desc\":\"" + e->description + "\"}";
            if (isSerialConnected()) Serial.println(payload);
            if (mqttConnected) {
                mqtt_publish(mqttCfg.topic, payload.c_str());
                lastDetectionTime = currentMillis;
                detectionActive = true;
            }
            detQueueTail = (detQueueTail + 1) % DET_QUEUE_SIZE;
            detQueueCount--;

            // Alert the user. Do it here in the main loop, not in the NimBLE
            // host callback or the WiFi driver ISR — blocking those tasks
            // with multi-hundred-ms delays destabilizes the stacks.
            startDetectionFlash();
            if (e->matchType[0] == 'N') {           // "NEW"
                threeBeeps();
            } else if (strncmp(e->matchType, "RE-30s", 6) == 0 ||
                       strncmp(e->matchType, "RE", 2) == 0) {
                threeBeeps();
            } else {                                // "RE-3s"
                twoBeeps();
            }
            continue;  // drain all before continuing
        }

        // Status report disabled - using JSON output only
        if (currentMillis - lastStatusTime >= 30000) {
            lastStatusTime = currentMillis;
        }

        mqtt_loop(currentMillis);
        delay(1);
        
        // Periodic heartbeat to confirm loop is alive
        static unsigned long lastHeartbeat = 0;
        if (currentMillis - lastHeartbeat >= 5000) {
            lastHeartbeat = currentMillis;
            if (isSerialConnected()) Serial.printf("[HEARTBEAT] phase=%d ch=%d dev=%zu\n", g_radioPhase, g_wifiChannel, devices.size());
        }
        }
    }

    // Update NeoPixel animation
    updateNeoPixelAnimation();
    
    delay(100);
} 