#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
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

// ================================
// Pin and Buzzer Definitions - Xiao ESP32 S3
// ================================
#define BUZZER_PIN 3   // GPIO3 (D2) for buzzer - good PWM pin on Xiao ESP32 S3
#define BUZZER_FREQ 2000  // Frequency in Hz
#define BUZZER_DUTY 127  // 50% duty cycle for good volume without excessive power draw
#define BEEP_DURATION 200  // Duration of each beep in ms
#define BEEP_PAUSE 150  // Pause between beeps in ms

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
#define AP_SSID "snoopuntothem"
#define AP_PASSWORD "astheysnoopuntous"
#define CONFIG_TIMEOUT 20000   // 20 seconds timeout for config mode

// ================================
// Operating Modes
// ================================
enum OperatingMode {
    CONFIG_MODE,
    SCANNING_MODE
};

// ================================
// Global Variables
// ================================
OperatingMode currentMode = CONFIG_MODE;
AsyncWebServer server(80);
Preferences preferences;
NimBLEScan* pBLEScan;
unsigned long configStartTime = 0;
unsigned long lastConfigActivity = 0;
unsigned long modeSwitchScheduled = 0; // When to switch modes (0 = not scheduled)
unsigned long deviceResetScheduled = 0; // When to reset device (0 = not scheduled)

// Serial output synchronization - avoid concurrent writes
volatile bool newMatchFound = false;
String detectedMAC = "";
int detectedRSSI = 0;
String matchedFilter = "";
String matchType = "";  // "NEW", "RE-5s", "RE-30s"

// Device tracking
struct DeviceInfo {
    String macAddress;
    int rssi;
    unsigned long firstSeen;
    unsigned long lastSeen;
    bool inCooldown;
    unsigned long cooldownUntil;
    const char* matchedFilter;
};

struct TargetFilter {
    String identifier;
    bool isFullMAC;
    String description;
};

std::vector<DeviceInfo> devices;
std::vector<TargetFilter> targetFilters;

// Forward declarations
void startScanningMode();
void startDetectionFlash();
class MyAdvertisedDeviceCallbacks;

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
// Buzzer Functions
// ================================
void initializeBuzzer() {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    ledcSetup(0, BUZZER_FREQ, 8);
    ledcAttachPin(BUZZER_PIN, 0);
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
    ledcWrite(0, BUZZER_DUTY);
    delay(BEEP_DURATION);
    ledcWrite(0, 0);
    digitalBeep(BEEP_DURATION);
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

void ascendingBeeps() {
    // Two fast ascending beeps to indicate "ready to scan"
    int frequencies[] = {1900, 2200}; // Close melodic interval, not octave
    int fastPause = 100; // Faster than normal beeps
    
    for (int i = 0; i < 2; i++) {
        ledcSetup(0, frequencies[i], 8);
        ledcWrite(0, BUZZER_DUTY);
        delay(BEEP_DURATION);
        ledcWrite(0, 0);
        if (i < 1) delay(fastPause);
    }
    
    // Reset to original frequency for future beeps
    ledcSetup(0, BUZZER_FREQ, 8);
}

// ================================
// Configuration Storage Functions
// ================================
void saveConfiguration() {
    preferences.begin("ouispy", false);
    preferences.putInt("filterCount", targetFilters.size());
    
    for (int i = 0; i < targetFilters.size(); i++) {
        String keyId = "id_" + String(i);
        String keyMAC = "mac_" + String(i);
        String keyDesc = "desc_" + String(i);
        
        preferences.putString(keyId.c_str(), targetFilters[i].identifier);
        preferences.putBool(keyMAC.c_str(), targetFilters[i].isFullMAC);
        preferences.putString(keyDesc.c_str(), targetFilters[i].description);
    }
    
    preferences.end();
    
    if (isSerialConnected()) {
        Serial.println("Configuration saved to NVS");
    }
}

void loadConfiguration() {
    preferences.begin("ouispy", true);
    int filterCount = preferences.getInt("filterCount", 0);
    
    targetFilters.clear();
    
    // Load saved filters or use defaults
    if (filterCount > 0) {
        for (int i = 0; i < filterCount; i++) {
            String keyId = "id_" + String(i);
            String keyMAC = "mac_" + String(i);
            String keyDesc = "desc_" + String(i);
            
            TargetFilter filter;
            filter.identifier = preferences.getString(keyId.c_str(), "");
            filter.isFullMAC = preferences.getBool(keyMAC.c_str(), false);
            filter.description = preferences.getString(keyDesc.c_str(), "");
            
            if (filter.identifier.length() > 0) {
                targetFilters.push_back(filter);
            }
        }
    } else {
        // Default configuration
        targetFilters.push_back({"AA:BB:CC", false, "Example Manufacturer"});
        targetFilters.push_back({"DD:EE:FF", false, "Another Manufacturer"});
        targetFilters.push_back({"AA:BB:CC:12:34:56", true, "Specific Device"});
    }
    
    preferences.end();
    
    if (isSerialConnected()) {
        Serial.println("Configuration loaded from NVS");
        Serial.println("Loaded " + String(targetFilters.size()) + " filters");
    }
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

bool matchesTargetFilter(const String& deviceMAC, String& matchedDescription) {
    String normalizedDeviceMAC = deviceMAC;
    normalizeMACAddress(normalizedDeviceMAC);
    
    for (const TargetFilter& filter : targetFilters) {
        String filterID = filter.identifier;
        normalizeMACAddress(filterID);
        
        if (filter.isFullMAC) {
            if (normalizedDeviceMAC.equals(filterID)) {
                matchedDescription = filter.description;
                return true;
            }
        } else {
            if (normalizedDeviceMAC.startsWith(filterID)) {
                matchedDescription = filter.description;
                return true;
            }
        }
    }
    return false;
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

const char* getConfigHTML() {
    return R"html(
<!DOCTYPE html>
<html>
<head>
    <title>OUI Spy Configuration</title>
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
        .ascii-background {
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            z-index: -1;
            opacity: 0.6;
            color: #ff1493;
            font-family: 'Courier New', monospace;
            font-size: 8px;
            line-height: 8px;
            white-space: pre;
            pointer-events: none;
            overflow: hidden;
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
            max-width: 700px; 
            margin: 0 auto; 
            background: #2d2d2d; 
            padding: 40px; 
            border-radius: 12px; 
            box-shadow: 0 4px 20px rgba(0,0,0,0.3); 
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
                width: 100%;
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
            width: 100%; 
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
        button { 
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); 
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
    </style>
</head>
<body>
    <div class="ascii-background">%ASCII_ART%</div>
    <div class="container">
        <h1>OUI-SPY</h1>
        
        <div class="status">
            Enter MAC addresses and/or OUI prefixes below. You must provide at least one entry in either field.
        </div>

        <form method="POST" action="/save">
            <div class="section">
                <h3>OUI Prefixes</h3>
                <textarea name="ouis" placeholder="Enter OUI prefixes, one per line:
AA:BB:CC
DD:EE:FF
11:22:33">%OUI_VALUES%</textarea>
                <div class="help-text">
                    OUI prefixes (first 3 bytes) match all devices from a manufacturer.<br>
                    Format: XX:XX:XX (8 characters with colons)
                </div>
            </div>
            
            <div class="section">
                <h3>MAC Addresses</h3>
                <textarea name="macs" placeholder="Enter full MAC addresses, one per line:
AA:BB:CC:12:34:56
DD:EE:FF:ab:cd:ef
11:22:33:44:55:66">%MAC_VALUES%</textarea>
                <div class="help-text">
                    Full MAC addresses match specific devices only.<br>
                    Format: XX:XX:XX:XX:XX:XX (17 characters with colons)
                </div>
            </div>
            
            <div class="button-container">
                <button type="submit">Save Configuration & Start Scanning</button>
                <button type="button" onclick="clearConfig()" style="background: #8b0000; margin-left: 20px;">Clear All Filters</button>
                <button type="button" onclick="deviceReset()" style="background: #4a0000; margin-left: 20px; font-size: 12px;">Device Reset</button>
            </div>
            
            <script>
            function clearConfig() {
                if (confirm('Are you sure you want to clear all filters? This action cannot be undone.')) {
                    document.getElementById('ouis').value = '';
                    document.getElementById('macs').value = '';
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
            </script>
        </form>
    </div>
</body>
</html>
)html";
}

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

String generateConfigHTML() {
    String html = getConfigHTML();
    String ouiValues = "";
    String macValues = "";
    
    // Populate existing saved values (if any)
    for (const TargetFilter& filter : targetFilters) {
        if (filter.isFullMAC) {
            if (macValues.length() > 0) macValues += "\n";
            macValues += filter.identifier;
        } else {
            if (ouiValues.length() > 0) ouiValues += "\n";
            ouiValues += filter.identifier;
        }
    }
    
    // Generate random examples for placeholders
    String randomOUIExamples = generateRandomOUI() + "\n" + generateRandomOUI() + "\n" + generateRandomOUI();
    String randomMACExamples = generateRandomMAC() + "\n" + generateRandomMAC() + "\n" + generateRandomMAC();
    
    // Replace static placeholders with random examples
    html.replace("AA:BB:CC\nDD:EE:FF\n11:22:33", randomOUIExamples);
    html.replace("AA:BB:CC:12:34:56\nDD:EE:FF:ab:cd:ef\n11:22:33:44:55:66", randomMACExamples);
    
    // Add ASCII art background
    html.replace("%ASCII_ART%", String(getASCIIArt()));
    
    html.replace("%OUI_VALUES%", ouiValues);
    html.replace("%MAC_VALUES%", macValues);
    return html;
}

// ================================
// WiFi and Web Server Functions
// ================================
void startConfigMode() {
    currentMode = CONFIG_MODE;
    // configStartTime will be set AFTER AP is fully ready
    
    Serial.println("\n=== STARTING CONFIG MODE ===");
    Serial.println("SSID: " + String(AP_SSID));
    Serial.println("Password: " + String(AP_PASSWORD));
    Serial.println("Initializing WiFi AP...");
    
    // Ensure WiFi is off first
    WiFi.mode(WIFI_OFF);
    delay(1000);
    
    // Start WiFi AP
    Serial.println("Setting WiFi mode to AP...");
    WiFi.mode(WIFI_AP);
    delay(500);
    
    Serial.println("Creating access point...");
    bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD);
    
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
    Serial.println("==============================\n");
    
    // NOW start the countdown - AP is fully ready and visible
    configStartTime = millis();
    lastConfigActivity = millis();
    
    // Setup web server routes
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        lastConfigActivity = millis();
        request->send(200, "text/html", generateConfigHTML());
    });
    
    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
        lastConfigActivity = millis();
        
        if (isSerialConnected()) {
            Serial.println("\n=== WEB CONFIG SUBMISSION ===");
        }
        
        targetFilters.clear();
        
        // Process OUI entries
        if (request->hasParam("ouis", true)) {
            String ouiData = request->getParam("ouis", true)->value();
            ouiData.trim();
            
            if (ouiData.length() > 0) {
                // Split by newlines and process each OUI
                int start = 0;
                int end = ouiData.indexOf('\n');
                
                while (start < ouiData.length()) {
                    String oui;
                    if (end == -1) {
                        oui = ouiData.substring(start);
                        start = ouiData.length();
                    } else {
                        oui = ouiData.substring(start, end);
                        start = end + 1;
                        end = ouiData.indexOf('\n', start);
                    }
                    
                    oui.trim();
                    oui.replace("\r", ""); // Remove carriage returns
                    
                    if (oui.length() > 0 && isValidMAC(oui)) {
                        TargetFilter filter;
                        filter.identifier = oui;
                        filter.description = "OUI: " + oui;
                        filter.isFullMAC = false;
                        targetFilters.push_back(filter);
                    }
                }
            }
        }
        
        // Process MAC address entries
        if (request->hasParam("macs", true)) {
            String macData = request->getParam("macs", true)->value();
            macData.trim();
            
            if (macData.length() > 0) {
                // Split by newlines and process each MAC
                int start = 0;
                int end = macData.indexOf('\n');
                
                while (start < macData.length()) {
                    String mac;
                    if (end == -1) {
                        mac = macData.substring(start);
                        start = macData.length();
                    } else {
                        mac = macData.substring(start, end);
                        start = end + 1;
                        end = macData.indexOf('\n', start);
                    }
                    
                    mac.trim();
                    mac.replace("\r", ""); // Remove carriage returns
                    
                    if (mac.length() > 0 && isValidMAC(mac)) {
                        TargetFilter filter;
                        filter.identifier = mac;
                        filter.description = "MAC: " + mac;
                        filter.isFullMAC = true;
                        targetFilters.push_back(filter);
                    }
                }
            }
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
        lastConfigActivity = millis();
        
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
        lastConfigActivity = millis();
        
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
    
    server.begin();
    
    if (isSerialConnected()) {
        Serial.println("Web server started!");
    }
}

// ================================
// BLE Advertised Device Callback Class
// ================================
class MyAdvertisedDeviceCallbacks: public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        if (currentMode != SCANNING_MODE) return;
        
        String mac = advertisedDevice->getAddress().toString().c_str();
        int rssi = advertisedDevice->getRSSI();
        unsigned long currentMillis = millis();

        String matchedDescription;
        bool matchFound = matchesTargetFilter(mac, matchedDescription);
        
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
                        // Store data for main loop to process
                        detectedMAC = mac;
                        detectedRSSI = rssi;
                        matchedFilter = matchedDescription;
                        matchType = "RE-30s";
                        newMatchFound = true;
                        
                        threeBeeps();
                        dev.inCooldown = true;
                        dev.cooldownUntil = currentMillis + 10000;
                    } else if (timeSinceLastSeen >= 5000) {
                        // Store data for main loop to process
                        detectedMAC = mac;
                        detectedRSSI = rssi;
                        matchedFilter = matchedDescription;
                        matchType = "RE-5s";
                        newMatchFound = true;
                        
                        twoBeeps();
                        dev.inCooldown = true;
                        dev.cooldownUntil = currentMillis + 5000;
                    }

                    dev.lastSeen = currentMillis;
                    break;
                }
            }

            if (!known) {
                DeviceInfo newDev = { mac, rssi, currentMillis, currentMillis, false, 0, matchedDescription.c_str() };
                devices.push_back(newDev);

                // Store data for main loop to process
                detectedMAC = mac;
                detectedRSSI = rssi;
                matchedFilter = matchedDescription;
                matchType = "NEW";
                newMatchFound = true;
                
                threeBeeps();
                
                auto& dev = devices.back();
                dev.inCooldown = true;
                dev.cooldownUntil = currentMillis + 5000;
            }
        }
    }
};

void startScanningMode() {
    currentMode = SCANNING_MODE;
    
    // Stop web server and WiFi
    server.end();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    
    if (isSerialConnected()) {
        Serial.println("\n=== STARTING SCANNING MODE ===");
        Serial.println("Configured Filters:");
        for (const TargetFilter& filter : targetFilters) {
            String type = filter.isFullMAC ? "Full MAC" : "OUI";
            Serial.println("- " + filter.identifier + " (" + type + "): " + filter.description);
        }
        Serial.println("==============================\n");
    }
    
    // Initialize BLE (but don't start scanning yet)
    NimBLEDevice::init("");
    delay(1000);
    
    // Setup BLE scanning (but don't start)
    pBLEScan = NimBLEDevice::getScan();
    if (pBLEScan != nullptr) {
        pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
        pBLEScan->setActiveScan(true);
        pBLEScan->setInterval(300);
        pBLEScan->setWindow(200);
    }
    
    // Ready to scan - ascending beeps (no interference possible)
    delay(500);
    ascendingBeeps();
    
    // 2-second pause after ready signal
    delay(2000);
    
    // NOW start BLE scanning - after ready signal is complete
    if (pBLEScan != nullptr) {
        pBLEScan->start(3, nullptr, false);
        
        if (isSerialConnected()) {
            Serial.println("BLE scanning started!");
        }
    }
}



// ================================
// Setup Function
// ================================
void setup() {
    delay(2000);
    
    // Initialize Serial first
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n=== OUI Spy Enhanced BLE Detector ===");
    Serial.println("Hardware: Xiao ESP32 S3");
    Serial.println("Buzzer: GPIO3 (D2)");
    Serial.println("NeoPixel: GPIO4 (D3)");
    Serial.println("Initializing...");
    
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
    
    // STEALTH MODE: Randomize ALL 6 bytes for maximum anonymity
    randomSeed(analogRead(0) + micros()); // Better randomization
    for (int i = 0; i < 6; i++) {
        newMAC[i] = random(0, 256);
    }
    // Ensure it's a valid locally administered address
    newMAC[0] |= 0x02; // Set locally administered bit
    newMAC[0] &= 0xFE; // Clear multicast bit
    
    // Set the randomized MAC for both STA and AP modes
    WiFi.mode(WIFI_STA);
    esp_wifi_set_mac(WIFI_IF_STA, newMAC);
    
    Serial.print("Randomized MAC: ");
    for (int i = 0; i < 6; i++) {
        if (newMAC[i] < 16) Serial.print("0");
        Serial.print(newMAC[i], HEX);
        if (i < 5) Serial.print(":");
    }
    Serial.println();
    
    // Silence ESP-IDF logs
    esp_log_level_set("*", ESP_LOG_NONE);
    
    Serial.println("Initializing buzzer...");
    initializeBuzzer();
    
    // Test buzzer
    Serial.println("Testing buzzer...");
    singleBeep();
    delay(500);
    
    Serial.println("Initializing NeoPixel...");
    initializeNeoPixel();
    
    // Test NeoPixel
    Serial.println("Testing NeoPixel...");
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
        
        // Clear in-memory filters
        targetFilters.clear();
        
        Serial.println("Factory reset complete - starting with clean state");
    } else {
        // Load configuration from NVS
        Serial.println("Loading configuration...");
        loadConfiguration();
    }
    
    // Start in configuration mode
    Serial.println("Starting configuration mode...");
    startConfigMode();
}

// ================================
// Loop Function
// ================================
void loop() {
    static unsigned long lastScanTime = 0;
    static unsigned long lastCleanupTime = 0;
    static unsigned long lastStatusTime = 0;
    unsigned long currentMillis = millis();
    
    if (currentMode == CONFIG_MODE) {
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
        
        // Check for config timeout 
        if (targetFilters.size() == 0) {
            // No saved filters - stay in config mode indefinitely
            if (currentMillis - configStartTime > CONFIG_TIMEOUT && lastConfigActivity == configStartTime) {
                if (isSerialConnected()) {
                    Serial.println("No one connected and no saved filters - staying in config mode");
                    Serial.println("Connect to 'snoopuntothem' AP to configure your first filters!");
                }
            }
        } else if (targetFilters.size() > 0) {
            // Have saved filters - timeout only if no one connected
            if (currentMillis - configStartTime > CONFIG_TIMEOUT && lastConfigActivity == configStartTime) {
                if (isSerialConnected()) {
                    Serial.println("No one connected within 20s - using saved filters, switching to scanning mode");
                }
                startScanningMode();
            } else if (lastConfigActivity > configStartTime) {
                // Someone connected - wait for them to submit (no timeout)
                if (isSerialConnected() && currentMillis - configStartTime > CONFIG_TIMEOUT) {
                    static unsigned long lastConnectedMsg = 0;
                    if (currentMillis - lastConnectedMsg > 30000) { // Print every 30s
                        Serial.println("Web interface connected - waiting for configuration submission...");
                        lastConnectedMsg = currentMillis;
                    }
                }
            }
        }
        
        // Handle web server
        delay(100);
        return;
    }
    
    // Scanning mode loop
    if (currentMode == SCANNING_MODE) {
        // Handle match detection messages (safe serial output)
        if (newMatchFound) {
            if (isSerialConnected()) {
                Serial.println(">> Match found! <<");
                Serial.print("Device: ");
                Serial.print(detectedMAC);
                Serial.print(" | RSSI: ");
                Serial.println(detectedRSSI);
                Serial.print("Filter: ");
                Serial.println(matchedFilter);
                
                if (matchType == "NEW") {
                    Serial.print("NEW DEVICE DETECTED: ");
                    Serial.println(matchedFilter);
                    Serial.print("MAC: ");
                    Serial.println(detectedMAC);
                } else if (matchType == "RE-30s") {
                    Serial.print("RE-DETECTED after 30+ sec: ");
                    Serial.println(matchedFilter);
                } else if (matchType == "RE-5s") {
                    Serial.print("RE-DETECTED after 5+ sec: ");
                    Serial.println(matchedFilter);
                }
                
                Serial.println("==============================");
            }
            newMatchFound = false;
        }
        
        // Restart BLE scan every 3 seconds
        if (currentMillis - lastScanTime >= 3000) {
            pBLEScan->stop();
            delay(10);
            pBLEScan->start(2, nullptr, false);
            lastScanTime = currentMillis;
        }

        // Clean up expired devices every 10 seconds
        if (currentMillis - lastCleanupTime >= 10000) {
            int devicesBefore = devices.size();
            
            for (auto it = devices.begin(); it != devices.end();) {
                if (currentMillis - it->lastSeen >= 60000) {
                    if (isSerialConnected()) {
                        Serial.println("Removed expired device: " + it->macAddress);
                    }
                    it = devices.erase(it);
                } else {
                    ++it;
                }
            }
            
            if (isSerialConnected() && devices.size() != devicesBefore) {
                Serial.println("Cleanup: Removed " + String(devicesBefore - devices.size()) + " expired devices");
            }
            
            lastCleanupTime = currentMillis;
        }

        // Status report every 30 seconds when USB connected
        if (isSerialConnected() && currentMillis - lastStatusTime >= 30000) {
            Serial.println("Status: Tracking " + String(devices.size()) + " active devices");
            lastStatusTime = currentMillis;
        }
    }
    
    // Update NeoPixel animation
    updateNeoPixelAnimation();
    
    delay(100);
} 