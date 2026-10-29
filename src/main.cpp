#include <Arduino.h>
#include <esp_ota_ops.h>
#include <nvs.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <NimBLEUtils.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include "mqtt.h"
#include "mesh.h"
#include "mesh_ota.h"
#include "devinfo.h"
#include "rid.h"
#include "wardrive.h"
#include <rom/crc.h>
#include <mbedtls/sha256.h>

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

// Boot sound: three ascending notes (C5 → E5 → G5), third held — a short
// fanfare instead of a long melody.

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

// Set while scanning with WiFi filters installed: the radio time-slices
// between promiscuous sweeps and BLE scans, so the STA only comes up during
// BLE phases (see the report flush in loop()). mqtt_loop() must not run its
// usual WiFi supervision/reconnect in that state — it would fight the radio.
// BLE-only scanning keeps STA up permanently and leaves this false.
bool g_mqttRadioTimeslice = false;
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
// (a driver/ISR-context callback) never touches the heap, and the head/count
// update runs inside a short critical section because the two producers run
// on different tasks.
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

// Raw detection report. Producers (BLE callback, WiFi promiscuous callback)
// only fill this and push it; everything heavier — device bookkeeping,
// cooldown, alerts, MQTT, session file — happens when loop() drains the ring.
struct DetectionEntry {
    char mac[18];
    char identifier[18];
    char description[64];
    int rssi;
    FilterType matchedType;
    bool wifiDomain;      // true = 802.11 MGMT frame match, false = BLE advert
    bool update;          // mesh-relayed signal update ("still here, this loud"): no alert
    uint8_t infoLen;      // device intel TLV (devinfo.h): name, services, SSID...
    uint8_t info[DEVINFO_MAX];
};

#define ALIAS_MAX 32   // node alias: plain ASCII, no quotes or backslashes
static bool aliasValid(const String& a) {
    if (a.length() > ALIAS_MAX) return false;
    for (size_t i = 0; i < a.length(); i++) {
        char c = a[i];
        if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') return false;
    }
    return true;
}

static const int DET_QUEUE_SIZE = 16;

static DetectionEntry detQueue[DET_QUEUE_SIZE];
static portMUX_TYPE detQueueMux = portMUX_INITIALIZER_UNLOCKED;
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
    unsigned long lastReport = 0;  // last report sent for it (alert or signal update)
};



// Helper: does this filter type belong to the WiFi domain? The domain is
// derived from the type — no separate field is stored on TargetFilter.
static inline bool isWifiDomain(FilterType t) { return t >= FT_WIFI_PROBE; }

// Three builds of this firmware (platformio.ini envs):
//   detector  the OUI-SPY Mesh node: filter matching; drone Remote ID off
//             unless turned on (console: rid on|off).
//   skyspy    a dedicated Sky-Spy drone node on the same mesh: Remote ID always
//             on, both radios always scanning, no filters needed.
//   wardrive  WiGLE-style: every access point and BLE device, printed for the
//             Tab5 (wardrive.h); both radios always scanning, no filters, scans
//             straight after a USB flash. Still a mesh node (names, OTA, settings).
// The kind is stamped into the image (the dashboard reads it before an update)
// and onto the build stamp every node reports ("Oct28 20:37:51S" = Sky-Spy,
// "...W" = wardriver).
#ifdef SKYSPY_NODE
#define FW_KIND_NAME "skyspy"
bool ridEnabled = true;
#elif defined(WARDRIVE_NODE)
#define FW_KIND_NAME "wardrive"
bool ridEnabled = true;   // drones are part of what's in the air
#else
#define FW_KIND_NAME "detector"
bool ridEnabled = false;
#endif
extern "C" const char FW_KIND[] = "OUISPY-KIND=" FW_KIND_NAME;

// Which radio domains the scan loop runs: what the filters need, plus both
// while Remote ID is on.
static void scanDomains(bool& wifi, bool& ble);

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
void saveConfiguration();
class MyAdvertisedDeviceCallbacks;
void enqueueDetection(const char* mac, const char* ident, const char* desc,
                      int rssi, FilterType type, bool wifi, const DevInfo* info);

// WiFi promiscuous mode forward declarations
static bool wifiOuiInTargets(const uint8_t oui[3]);
static void rebuildWifiOuiTable();
static void handleWifiMatch(const uint8_t mac[6], uint8_t subtype, int8_t rssi);

// Live filter edits (a mesh push, add or remove) change targetFilters while
// the radio runs. The BLE and WiFi callbacks register as readers before
// they touch the list and skip the frame while an edit is in progress; the
// editor (loop context only) raises the flag, waits for the readers in
// flight to finish, swaps the list and drops the flag. Order matters: a
// reader counts itself in BEFORE it checks the flag, the editor raises the
// flag BEFORE it checks the count, so one of them always sees the other.
static std::atomic<int>  filterReaders{0};
static std::atomic<bool> filterEditing{false};
struct FilterReadGuard {
    bool ok;
    FilterReadGuard() { filterReaders.fetch_add(1); ok = !filterEditing.load(); }
    ~FilterReadGuard() { filterReaders.fetch_sub(1); }
};
static void filterEditBegin() {
    filterEditing.store(true);
    while (filterReaders.load() > 0) delay(1);
}
static void filterEditEnd() {
    rebuildWifiOuiTable();
    filterEditing.store(false);
}
static bool liveEditNeedsRestart();

// LittleFS session forwards (definitions live further down)
static void ensureSessionFS();
static void rotateSessionFiles();
static void writeCurrentSession();
static void clearCurrentSessionFile();


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
// Serial provisioning console
// ================================
// One-line commands typed (or scripted) over USB. meshrechan/preset/meshpush
// deliberately work in ANY mode — USB access already implies full control
// (reflashing is always possible), so a burned-in node shouldn't need a
// button dance for routine fleet operations. Everything else requires the
// portal (config mode).
//
//   mesh <0|1> <home|remote> <ch>     e.g. "mesh 1 remote 6"
//   filter <oui> <ble> <wifi> <wtype> <desc...>   e.g. "filter 0025DF 0 1 probe AXON"
//   preset <axon|meta|ipro|verkada>   install a full vendor signature set + reboot
//   meshpush                          flood the current filter list to the fleet
//   meshrechan <ch>                   re-channel the whole fleet, any mode
//   meshmute [all] / meshunmute       fleet buzzer (and LED) off / on, any mode
//   meshkey <secret|->                set / clear the fleet key, any mode
//   clearfilters
//   provisiondone                     save + switch to scanning
struct PresetEntry;
int applyPreset(const PresetEntry* preset, size_t count, const char* labelPrefix);
extern const PresetEntry PRESET_AXON[];
extern const size_t PRESET_AXON_COUNT;
extern const PresetEntry PRESET_META[];
extern const size_t PRESET_META_COUNT;
extern const PresetEntry PRESET_IPRO[];
extern const size_t PRESET_IPRO_COUNT;
extern const PresetEntry PRESET_VERKADA[];
extern const size_t PRESET_VERKADA_COUNT;

// THE AP-visibility bug: the mesh turns on WIFI_PROTOCOL_LR, and the driver
// lets LR land on the AP interface too (apcheck showed ap=0xF). An LR AP
// beacons in Espressif-only LR, so the driver reports a perfect AP that no
// phone or laptop can decode. Call after every softAP() start: the portal
// AP always runs plain 802.11 b/g/n; the mesh keeps LR on its STA side.
static void apForcePlainProtocol() {
    esp_err_t e = esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    uint8_t ap = 0;
    esp_wifi_get_protocol(WIFI_IF_AP, &ap);
    Serial.printf("AP protocol forced to b/g/n: %s (ap=0x%X)\n", esp_err_to_name(e), ap);
}

static bool remoteConsoleCommand(const String& line);   // remote management, below
// Filters a node holds (every preset in the library is ~160 rows).
#define FILTER_MAX 250
static uint16_t serializeFilters(uint8_t* buf, uint16_t cap, bool& tooBig, bool withDesc = true);

// Fleet command issued from the console, so its ack count can be reported
// once remotes have had time to answer (0 = none outstanding).
static uint16_t g_consoleCmdId = 0;
static unsigned long g_consoleCmdAt = 0;
static const char* g_consoleCmdName = "";

static void serialConsoleTick() {
    if (g_consoleCmdId && millis() - g_consoleCmdAt > 4000) {
        Serial.printf("[MESH] %s confirmed by %u remote(s)\n", g_consoleCmdName, meshCmdAckCount(g_consoleCmdId));
        g_consoleCmdId = 0;
    }
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;

    // Dashboard / fleet management commands (status, remote settings,
    // filter transfers) work in ANY mode — see remoteConsoleCommand.
    if (remoteConsoleCommand(line)) return;

    // Locked-node fleet management from ANY mode (USB = full control):
    if (line.startsWith("meshrechan ")) {
        int ch = line.substring(11).toInt();
        if (ch < 1 || ch > 11) { Serial.println("ERR channel 1-11"); return; }
        if (currentMode == CONFIG_MODE) {
            // Config mode: flood and let the operator finish with
            // burnin/provisiondone — this node reboots on the new channel
            // whenever they do.
            uint8_t p[1] = { (uint8_t)ch };
            uint16_t cmdId = meshNewCmdId();
            if (!meshQueueCommand(MESH_CMD_SETCHAN, cmdId, p, 1)) {
                Serial.println("ERR mesh TX not running");
                return;
            }
            meshFlushTxNow();   // must leave NOW, while parked on the old channel
            meshCfg.channel = (uint8_t)ch;
            mesh_saveConfig();
            Serial.printf("OK mesh re-channeled to %d — command flooded, follow with burnin\n", ch);
            return;
        }
        // Scanning mode (incl. burned-in): flood, wait for a mesh window so
        // the command actually leaves, then reboot onto the new channel.
        uint8_t p[1] = { (uint8_t)ch };
        if (!meshQueueCommand(MESH_CMD_SETCHAN, meshNewCmdId(), p, 1)) {
            Serial.println("ERR mesh TX not running");
            return;
        }
        meshCfg.channel = (uint8_t)ch;
        mesh_saveConfig();
        Serial.printf("OK re-channeled fleet to %d — rebooting onto it\n", ch);
        Serial.flush();
        for (int i = 0; i < 40 && meshTxPending() > 0; i++) {
            meshFlushTxNow();
            delay(100);
        }
        delay(300);
        ESP.restart();
        return;
    }

    // Fleet audio from USB, any mode (a burned-in home has no portal):
    //   meshmute        remotes' buzzers off (LEDs stay on)
    //   meshmute all    buzzer and LED off
    //   meshunmute      both back on
    // Same MESH_CMD_AUDIO as the portal's Mute/Unmute All Remotes; live and
    // persisted on the remotes. This node's own audio is untouched.
    if (line == "meshmute" || line == "meshmute all" || line == "meshunmute") {
        if (!meshCfg.enabled) { Serial.println("ERR mesh is off on this node"); return; }
        bool unmute = (line == "meshunmute");
        uint8_t p[2] = { (uint8_t)(unmute ? 1 : 0), (uint8_t)((unmute || line == "meshmute") ? 1 : 0) };
        uint16_t cmdId = meshNewCmdId();
        if (!meshQueueCommand(MESH_CMD_AUDIO, cmdId, p, 2)) {
            Serial.println("ERR mesh TX busy or down, try again");
            return;
        }
        g_consoleCmdId = cmdId;
        g_consoleCmdAt = millis();
        g_consoleCmdName = unmute ? "unmute" : (p[1] ? "mute (buzzer)" : "mute (buzzer + LED)");
        Serial.printf("OK %s flooded to the fleet — ack count in 4 s\n", g_consoleCmdName);
        return;
    }

    // Fleet key: "meshkey <secret>" sets it (8-63 chars, no spaces),
    // "meshkey -" clears it. Same key on every node; with a key set this
    // node signs its commands and only obeys commands signed with it.
    if (line.startsWith("meshkey ")) {
        String k = line.substring(8);
        k.trim();
        if (k == "-") {
            meshSetKey("");
            Serial.println("OK fleet key cleared — commands are unauthenticated");
            return;
        }
        if (k.length() < 8 || k.length() > 63 || k.indexOf(' ') >= 0) {
            Serial.println("ERR key must be 8-63 characters, no spaces (or - to clear)");
            return;
        }
        meshSetKey(k.c_str());
        Serial.println("OK fleet key set — set the same key on every node");
        return;
    }

    // Install a full vendor signature set (BLE OUI + CID + service UUID +
    // WiFi probe/beacon as applicable) on THIS node, save, reboot. Works in
    // any mode so a burned-in node can be re-signatured without unlocking.
    if (line.startsWith("preset ")) {
        String name = line.substring(7);
        name.toLowerCase();
        const PresetEntry* table = nullptr;
        size_t count = 0;
        const char* label = nullptr;
        if (name == "axon")    { table = PRESET_AXON;    count = PRESET_AXON_COUNT;    label = "Axon body cam"; }
        else if (name == "meta")    { table = PRESET_META;    count = PRESET_META_COUNT;    label = "META / RAY-BAN"; }
        else if (name == "ipro")    { table = PRESET_IPRO;    count = PRESET_IPRO_COUNT;    label = "I-PRO body cam"; }
        else if (name == "verkada") { table = PRESET_VERKADA; count = PRESET_VERKADA_COUNT; label = "Verkada camera"; }
        if (!table) { Serial.println("ERR unknown preset (axon|meta|ipro|verkada)"); return; }
        filterEditBegin();
        int added = applyPreset(table, count, label);
        filterEditEnd();
        saveConfiguration();
        if (liveEditNeedsRestart()) {
            Serial.printf("OK preset %s installed (%d signatures) — rebooting to start the BLE scanner\n", name.c_str(), added);
            Serial.flush();
            delay(300);
            ESP.restart();
        }
        Serial.printf("OK preset %s installed live (%d signatures)\n", name.c_str(), added);
        return;
    }

    // Flood the CURRENT filter list to every node in the fleet: receivers
    // stage it, CRC it, write it to NVS and reboot into it (the existing
    // MESH_CMD_FILTERS path — receivers need no new firmware).
    if (line == "meshpush") {
        if (targetFilters.size() == 0) { Serial.println("ERR no filters installed"); return; }
        uint8_t* buf = (uint8_t*)malloc(4096);
        if (!buf) { Serial.println("ERR out of memory"); return; }
        bool tooBig = false;
        uint16_t n = serializeFilters(buf, 4096, tooBig);
        uint16_t cmdId = meshNewCmdId();
        bool ok = !tooBig && meshQueueCommand(MESH_CMD_FILTERS, cmdId, buf, n);
        free(buf);
        if (!ok) { Serial.println("ERR too many filters or mesh TX down"); return; }
        Serial.printf("OK filter push flooded to fleet (cmd %d, %d records) — acks in heartbeat\n", cmdId, targetFilters.size());
        Serial.printf("CMD {\"op\":\"meshpush\",\"id\":%u,\"target\":\"all\",\"cmd\":%u}\n", cmdId, MESH_CMD_FILTERS);
        return;
    }

    if (currentMode != CONFIG_MODE) return;

    if (line.startsWith("mesh ")) {
        int s1 = line.indexOf(' ');
        int s2 = line.indexOf(' ', s1 + 1);
        int s3 = line.indexOf(' ', s2 + 1);
        if (s2 > 0 && s3 > 0) {
            meshCfg.enabled = line.substring(s1 + 1, s2).toInt() != 0;
            meshCfg.home    = line.substring(s2 + 1, s3).equalsIgnoreCase("home");
            int ch = line.substring(s3 + 1).toInt();
            if (ch >= 1 && ch <= 11) meshCfg.channel = (uint8_t)ch;
            Serial.printf("OK mesh %s %s ch%d\n", meshCfg.enabled ? "on" : "off",
                          meshCfg.home ? "home" : "remote", meshCfg.channel);
        } else {
            Serial.println("ERR usage: mesh <0|1> <home|remote> <ch>");
        }
        return;
    }

    if (line.startsWith("filter ")) {
        // filter <oui> <ble 0/1> <wifi 0/1> <probe|beacon> <desc...>
        int p1 = line.indexOf(' ');
        int p2 = line.indexOf(' ', p1 + 1);
        int p3 = line.indexOf(' ', p2 + 1);
        int p4 = line.indexOf(' ', p3 + 1);
        if (p4 > 0) {
            String oui = line.substring(p1 + 1, p2);
            oui.toUpperCase();
            oui.replace(":", "");
            bool ble  = line.substring(p2 + 1, p3).toInt() != 0;
            bool wifi = line.substring(p3 + 1, p4).toInt() != 0;
            String rest = line.substring(p4 + 1);
            int sp = rest.indexOf(' ');
            String wtype = (sp > 0) ? rest.substring(0, sp) : rest;
            String desc  = (sp > 0) ? rest.substring(sp + 1) : "";
            if ((oui.length() == 6 || oui.length() == 12) && (ble || wifi)) {
                TargetFilter f;
                f.identifier = oui;
                bool fullMac = (oui.length() == 12);
                f.isFullMAC = fullMac;
                String formatted = fullMac ? oui
                    : oui.substring(0,2) + ":" + oui.substring(2,4) + ":" + oui.substring(4,6);
                if (fullMac) {
                    f.type = FT_FULL_MAC;
                    f.description = desc.isEmpty() ? "MAC: " + formatted : desc;
                } else if (ble) {
                    f.type = FT_MAC_PREFIX;
                    f.description = desc.isEmpty() ? "OUI: " + formatted : desc;
                } else {
                    f.type = (wtype.equalsIgnoreCase("beacon")) ? FT_WIFI_BEACON : FT_WIFI_PROBE;
                    f.description = desc.isEmpty() ? "OUI: " + formatted + " (WiFi)" : desc;
                }
                targetFilters.push_back(f);
                Serial.printf("OK filter %s (%s) total=%d\n", formatted.c_str(),
                              filterTypeCode(f.type), targetFilters.size());
            } else {
                Serial.println("ERR bad oui length or no domain");
            }
        } else {
            Serial.println("ERR usage: filter <oui> <ble> <wifi> <probe|beacon> <desc>");
        }
        return;
    }

    if (line == "apcheck") {
        // Diagnose AP visibility complaints: what does the driver think it
        // is beaconing? (config mode only)
        wifi_config_t cfg;
        if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK) {
            Serial.printf("AP ssid=%.32s ssid_len=%d ch=%d auth=%d maxconn=%d\n",
                          cfg.ap.ssid, cfg.ap.ssid_len, cfg.ap.channel, cfg.ap.authmode, cfg.ap.max_connection);
        } else {
            Serial.println("ERR ap get_config failed");
        }
        Serial.printf("AP ip=%s stations=%d\n",
                      WiFi.softAPIP().toString().c_str(),
                      WiFi.softAPgetStationNum());
        {
            uint8_t curCh = 0;
            wifi_second_chan_t secCh = WIFI_SECOND_CHAN_NONE;
            esp_wifi_get_channel(&curCh, &secCh);
            int8_t txp = 0;
            esp_wifi_get_max_tx_power(&txp);
            wifi_mode_t mode;
            esp_wifi_get_mode(&mode);
            Serial.printf("RADIO mode=%d ACTUAL channel=%d (configured %d) tx_power=%d (%.1f dBm)\n",
                          (int)mode, curCh, cfg.ap.channel, txp, txp * 0.25f);
            // Protocol masks: 1=11b 2=11g 4=11n 8=LR. An LR bit on the AP
            // side means beacons may be going out in Espressif-only LR.
            uint8_t sp = 0, ap = 0;
            esp_wifi_get_protocol(WIFI_IF_STA, &sp);
            esp_wifi_get_protocol(WIFI_IF_AP, &ap);
            Serial.printf("PROTO sta=0x%X ap=0x%X (1=b 2=g 4=n 8=LR)\n", sp, ap);
        }
        return;
    }

    if (line.startsWith("proto ")) {
        // Live A/B for the AP-visibility bug: proto <sta_hex> <ap_hex>,
        // e.g. "proto f 7" (STA LR, AP plain bgn) or "proto 7 7" (no LR).
        int split = line.indexOf(' ', 6);
        if (split > 0) {
            uint8_t sta = (uint8_t)strtol(line.substring(6, split).c_str(), nullptr, 16);
            uint8_t ap = (uint8_t)strtol(line.substring(split + 1).c_str(), nullptr, 16);
            esp_err_t e1 = esp_wifi_set_protocol(WIFI_IF_STA, sta);
            esp_err_t e2 = esp_wifi_set_protocol(WIFI_IF_AP, ap);
            uint8_t gs = 0, ga = 0;
            esp_wifi_get_protocol(WIFI_IF_STA, &gs);
            esp_wifi_get_protocol(WIFI_IF_AP, &ga);
            Serial.printf("proto set sta=0x%X(%s) ap=0x%X(%s) -> now sta=0x%X ap=0x%X\n",
                          sta, esp_err_to_name(e1), ap, esp_err_to_name(e2), gs, ga);
        } else {
            Serial.println("ERR usage: proto <sta_hex> <ap_hex>   (1=b 2=g 4=n 8=LR)");
        }
        return;
    }

    if (line == "wifiscan") {
        Serial.println("scanning (3s)...");
        int n = WiFi.scanNetworks(false, true);  // async=false, show hidden
        for (int i = 0; i < n && i < 12; i++) {
            Serial.printf("%2d  %-32.32s ch%2d  %d dBm  %s\n", i + 1,
                          WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i),
                          WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "SEC");
        }
        if (n == 0) Serial.println("(no networks seen — RX path suspect)");
        WiFi.scanDelete();
        return;
    }

    if (line == "meshoff") {
        // Definitive ESP-NOW/softAP conflict test: tear the mesh down and
        // re-run the AP cleanly. If the AP becomes visible, the coexistence
        // is the beacon-killer.
        mesh_deinit();
        delay(200);
        WiFi.mode(WIFI_AP);
        delay(200);
        String apSsid = AP_SSID.length() > 0 ? AP_SSID : "snoopuntothem";
        String apPass = AP_PASSWORD.length() > 0 ? AP_PASSWORD : "astheysnoopuntous";
        bool ok = WiFi.softAP(apSsid.c_str(), apPass.c_str(), 1);
        apForcePlainProtocol();
        Serial.printf("OK mesh torn down, AP re-created=%d ch=1 — SCAN YOUR PHONE NOW\n", (int)ok);
        return;
    }

    if (line.startsWith("jtest ")) {
        // Decisive TX-alive test: try to JOIN a network. Joining requires
        // the transmitter to work (auth frames, ACKs); RX alone can't fake
        // it. Usage: jtest <ssid> <password>   (no spaces in ssid)
        int sp1 = line.indexOf(' ');
        int sp2 = line.indexOf(' ', sp1 + 1);
        if (sp2 < 0) { Serial.println("ERR usage: jtest <ssid> <pass>"); return; }
        String ssid = line.substring(sp1 + 1, sp2);
        String pass = line.substring(sp2 + 1);
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());
        Serial.printf("joining %s ...\n", ssid.c_str());
        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(250);
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("OK joined %s ip=%s — TX IS ALIVE\n", ssid.c_str(), WiFi.localIP().toString().c_str());
        } else {
            Serial.println("ERR join failed — TX suspect (or bad creds)");
        }
        WiFi.disconnect(true);
        delay(300);
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID.c_str(), AP_PASSWORD.c_str(), 1);
        apForcePlainProtocol();
        return;
    }

    if (line == "clearfilters") {
        targetFilters.clear();
        Serial.println("OK filters cleared");
        return;
    }

    if (line.startsWith("mqtt ")) {
        // Headless MQTT staging for burn-in: k=v tokens (no spaces in values).
        //   mqtt en=1 ssid=Net pass=secret bk=192.168.1.10 pt=1883 us=u pw=p id=ouispy-garage
        int sp = line.indexOf(' ');
        String rest = line.substring(sp + 1);
        int start = 0;
        while (start < (int)rest.length()) {
            int sp2 = rest.indexOf(' ', start);
            String tok = (sp2 < 0) ? rest.substring(start) : rest.substring(start, sp2);
            int eq = tok.indexOf('=');
            if (eq > 0) {
                String k = tok.substring(0, eq);
                String v = tok.substring(eq + 1);
                if (k == "en")   mqttCfg.enabled = (v.toInt() != 0);
                else if (k == "ssid") strlcpy(mqttCfg.sta_ssid, v.c_str(), sizeof(mqttCfg.sta_ssid));
                else if (k == "pass") strlcpy(mqttCfg.sta_pass, v.c_str(), sizeof(mqttCfg.sta_pass));
                else if (k == "bk")   strlcpy(mqttCfg.broker, v.c_str(), sizeof(mqttCfg.broker));
                else if (k == "pt")   mqttCfg.port = (uint16_t)v.toInt();
                else if (k == "us")   strlcpy(mqttCfg.user, v.c_str(), sizeof(mqttCfg.user));
                else if (k == "pw")   strlcpy(mqttCfg.pass, v.c_str(), sizeof(mqttCfg.pass));
                else if (k == "id")   strlcpy(mqttCfg.device_id, v.c_str(), sizeof(mqttCfg.device_id));
                else if (k == "tp")   strlcpy(mqttCfg.topic, v.c_str(), sizeof(mqttCfg.topic));
            }
            if (sp2 < 0) break;
            start = sp2 + 1;
        }
        if (mqttCfg.port == 0) mqttCfg.port = 1883;
        if (mqttCfg.device_id[0] == 0) strlcpy(mqttCfg.device_id, "ouispy", sizeof(mqttCfg.device_id));
        if (mqttCfg.topic[0] == 0) snprintf(mqttCfg.topic, sizeof(mqttCfg.topic), "%s/detection", mqttCfg.device_id);
        mqtt_saveConfig();
        Serial.printf("OK mqtt saved (en=%d broker=%s:%d id=%s)\n",
                      mqttCfg.enabled ? 1 : 0, mqttCfg.broker, mqttCfg.port, mqttCfg.device_id);
        return;
    }

    if (line == "provisiondone") {
        // A mesh node may start with no filters (the web flasher sets them
        // up that way): it joins the mesh and gets its list from the
        // dashboard. Without the mesh it would have nothing to do.
        if (targetFilters.size() == 0 && !meshCfg.enabled && !wdOn()) {
            Serial.println("ERR no filters installed");
            return;
        }
        mesh_saveConfig();
        saveConfiguration();
        rebuildWifiOuiTable();
        Serial.println("OK saved — switching to scanning");
        Serial.flush();
        delay(300);
        startScanningMode();
        return;
    }

    if (line == "burnin") {
        // Same end state as the portal's Burn In button: everything staged
        // gets persisted, the lock is set, and the node boots straight into
        // scanning from here on (BOOT-hold at power-on to unlock).
        if (targetFilters.size() == 0 && !meshCfg.enabled && !wdOn()) {
            Serial.println("ERR no filters installed");
            return;
        }
        mesh_saveConfig();
        saveConfiguration();
        rebuildWifiOuiTable();
        preferences.begin("ouispy", false);
        preferences.putBool("configLocked", true);
        preferences.end();
        Serial.println("OK burned in — rebooting into locked scanning mode");
        Serial.flush();
        delay(500);
        ESP.restart();
        return;
    }

    Serial.println("ERR unknown command (mesh|filter|clearfilters|provisiondone|burnin)");
}

// ================================
// LED Control Functions (inverted logic for Xiao ESP32-S3)
// ================================
unsigned long lastLedActivity = 0;   // set by ledOn/ledOff — blink sequences

void ledOn() {
    lastLedActivity = millis();
    if (ledEnabled) {
        digitalWrite(LED_PIN, LOW);  // LOW = LED ON for Xiao ESP32-S3
    }
}

void ledOff() {
    lastLedActivity = millis();
    if (ledEnabled) {
        digitalWrite(LED_PIN, HIGH); // HIGH = LED OFF for Xiao ESP32-S3
    }
}

// Mesh link heartbeat on the onboard LED: solid ON while at least one mesh
// node is in the roster, OFF when the node is alone. Detection blinks (the
// ledOn/ledOff calls inside the beep sequences) override it temporarily;
// the steady state is re-asserted once the blink goes quiet. Unmeshed nodes
// keep legacy behavior (LED only blinks with detections).
static void meshLinkLedTick() {
    if (!meshCfg.enabled) return;
    if (millis() - lastLedActivity < 120) return;  // a detection blink is in progress
    digitalWrite(LED_PIN, (meshLinkFresh() && ledEnabled) ? LOW : HIGH);
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
    // Power-on fanfare: four ascending notes (C5 → E5 → G5 → C6), quick and
    // bright. Plays ONCE, immediately at power-on; the confirm jingle at the
    // end of startScanningMode is the only other half of the boot sound.
    const uint16_t freq[] = {523, 659, 784, 1047};
    const uint16_t dur[]  = {80, 80, 110, 230};
    for (int i = 0; i < 4; i++) {
        if (buzzerEnabled) {
            ledcSetup(0, freq[i], 8);
            ledcWrite(0, BUZZER_DUTY);
        }
        ledOn();
        delay(dur[i]);
        if (buzzerEnabled) {
            ledcWrite(0, 0);
        }
        ledOff();
        if (i < 3) delay(40);
    }
    // Reset to original frequency for future detection beeps
    if (buzzerEnabled) {
        ledcSetup(0, BUZZER_FREQ, 8);
    }
}

void ascendingBeeps() {
    // "Ready to scan" — the full Mario power-up run: six fast ascending
    // notes, ~370 ms total. Plays at the END of startScanningMode, right as
    // the first sweep starts, so its last note means "actually scanning".
    const uint16_t run[] = {523, 659, 784, 1047, 1319, 1568}; // C5 E5 G5 C6 E6 G6
    const int noteDur = 55;

    for (int i = 0; i < 6; i++) {
        if (buzzerEnabled) {
            ledcSetup(0, run[i], 8);
            ledcWrite(0, BUZZER_DUTY);
        }
        ledOn();
        delay(noteDur);
        if (buzzerEnabled) {
            ledcWrite(0, 0);
        }
        ledOff();
        if (i < 5) delay(12);
    }
    
    // Reset to original frequency for future beeps
    if (buzzerEnabled) {
        ledcSetup(0, BUZZER_FREQ, 8);
    }
}

// ================================
// Configuration Storage Functions
// ================================
// Filters live in NVS as ONE blob in the mesh wire format (type, flags,
// idLen, descLen, id, desc per record): ~40 bytes a filter. The old layout
// spent four keys (~6 NVS entries, ~190 bytes) per filter and never erased
// a shrunk list's leftovers, so the 20 KB partition filled up around 80-100
// filters. Legacy keys are read once, converted, and erased.
static bool filterRecordsValid(const uint8_t* buf, uint16_t len, uint8_t& countOut);
static TargetFilter filterFromRecord(const uint8_t* buf, uint16_t& off);
#define FILTER_BLOB_KEY "fblob"
// A freshly updated image is on probation until it confirms itself (see
// otaRollbackTick); if it fails, the bootloader rolls back to the previous
// build, which only reads the old per-filter keys. So those keys are kept
// until this image is confirmed: a rollback never comes up with no filters.
static bool imageOnProbation() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}
static void eraseLegacyFilterKeys(int upto) {
    for (int i = 0; i < upto; i++) {
        String n = String(i);
        preferences.remove(("id_" + n).c_str());
        preferences.remove(("mac_" + n).c_str());
        preferences.remove(("desc_" + n).c_str());
        preferences.remove(("type_" + n).c_str());
    }
}

// Filter list storage: a file on LittleFS (/filters.bin, written to a temp
// file and renamed, so a power cut never leaves half a list). NVS is 20 KB
// and shared with the mesh boot/sequence counters: a 160-filter blob there
// (6.5 KB, plus room to rewrite it) filled it, later writes failed silently,
// a node rebooted with an old list and a stuck boot counter (every frame then
// read as a replay). NVS keeps only small settings; while a new image is on
// probation it also keeps the blob, as what a rollback build reads.
#define FILTER_FILE "/filters.bin"
#define FILTER_TMP  "/filters.tmp"
static bool filterFsReady();
static bool writeFilterFile(const uint8_t* buf, uint16_t n) {
    if (!filterFsReady()) return false;
    File f = LittleFS.open(FILTER_TMP, "w");
    if (!f) return false;
    size_t w = f.write(buf, n);
    f.close();
    if (w != n) { LittleFS.remove(FILTER_TMP); return false; }
    LittleFS.remove(FILTER_FILE);
    return LittleFS.rename(FILTER_TMP, FILTER_FILE);
}
// Reads the file into a malloc'd buffer (caller frees); 0 = none.
static size_t readFilterFile(uint8_t** out) {
    *out = nullptr;
    if (!filterFsReady() || !LittleFS.exists(FILTER_FILE)) return 0;
    File f = LittleFS.open(FILTER_FILE, "r");
    if (!f) return 0;
    size_t n = f.size();
    if (n == 0 || n > FILTER_MAX * (4 + 16 + 48)) { f.close(); return 0; }
    *out = (uint8_t*)malloc(n);
    if (!*out) { f.close(); return 0; }
    size_t r = f.read(*out, n);
    f.close();
    if (r != n) { free(*out); *out = nullptr; return 0; }
    return n;
}

void saveConfiguration() {
    preferences.begin("ouispy", false);
    preferences.putBool("buzzerEnabled", buzzerEnabled);
    preferences.putBool("ledEnabled", ledEnabled);

    const uint16_t cap = FILTER_MAX * (4 + 16 + 48);
    uint8_t* buf = (uint8_t*)malloc(cap);
    bool saved = false;
    if (buf) {
        bool tooBig;
        uint16_t n = serializeFilters(buf, cap, tooBig);
        saved = writeFilterFile(buf, n);
        bool probation = imageOnProbation();
        if (!saved || probation) {
            // No file system (fallback), or the rollback build's copy. The
            // copy only goes in with room to spare: the mesh boot/sequence
            // counters must always find free entries (each entry is 32 B).
            nvs_stats_t st;
            bool room = nvs_get_stats(NULL, &st) == ESP_OK && st.free_entries > (size_t)(n / 32 + 64);
            bool nvsOk = room && preferences.putBytes(FILTER_BLOB_KEY, buf, n) == n;
            if (!room && saved) preferences.remove(FILTER_BLOB_KEY);   // a stale, oversized copy only crowds NVS
            saved = saved || nvsOk;
        } else {
            preferences.remove(FILTER_BLOB_KEY);     // the file is the list now
        }
        free(buf);
    }
    preferences.end();

    if (isSerialConnected()) {
        Serial.println(saved ? "Configuration saved" : "[CFG] could not save the filter list (file system and NVS both failed)");
    }
}

void loadConfiguration() {
    preferences.begin("ouispy", true);
    buzzerEnabled = preferences.getBool("buzzerEnabled", true);
    ledEnabled = preferences.getBool("ledEnabled", true);
#if !defined(SKYSPY_NODE) && !defined(WARDRIVE_NODE)
    ridEnabled = preferences.getBool("rid", false);
#endif
    targetFilters.clear();

    auto loadRecords = [](const uint8_t* buf, size_t len) {
        uint8_t count = 0;
        if (!filterRecordsValid(buf, len, count)) return false;
        uint16_t off = 0;
        for (uint8_t i = 0; i < count; i++) targetFilters.push_back(filterFromRecord(buf, off));
        return true;
    };
    bool migrate = false;
    uint8_t* fbuf = nullptr;
    size_t flen = readFilterFile(&fbuf);
    if (flen && loadRecords(fbuf, flen)) {
        // the file is the list
    } else if (preferences.isKey(FILTER_BLOB_KEY) && preferences.getBytesLength(FILTER_BLOB_KEY) > 0) {
        size_t blobLen = preferences.getBytesLength(FILTER_BLOB_KEY);
        uint8_t* buf = (uint8_t*)malloc(blobLen);
        if (buf) {
            preferences.getBytes(FILTER_BLOB_KEY, buf, blobLen);
            loadRecords(buf, blobLen);
            free(buf);
        }
        migrate = true;                      // NVS blob -> file
    } else {
        // Oldest layout: four keys per filter.
        int filterCount = preferences.getInt("filterCount", 0);
        for (int i = 0; i < filterCount; i++) {
            String n = String(i);
            TargetFilter filter;
            filter.identifier  = preferences.getString(("id_" + n).c_str(), "");
            filter.isFullMAC   = preferences.getBool  (("mac_" + n).c_str(), false);
            filter.description = preferences.getString(("desc_" + n).c_str(), "");
            uint8_t rawType = preferences.getUChar(("type_" + n).c_str(), 0xFF);
            if (rawType == 0xFF)               filter.type = filter.isFullMAC ? FT_FULL_MAC : FT_MAC_PREFIX;
            else if (rawType <= FT_WIFI_BEACON) filter.type = (FilterType)rawType;
            else                                filter.type = FT_MAC_PREFIX;   // corrupt value, fail safe
            if (filter.identifier.length() > 0) targetFilters.push_back(filter);
        }
        migrate = filterCount > 0;
    }
    if (fbuf) free(fbuf);
    preferences.end();
    if (migrate) saveConfiguration();   // to the file (NVS copies go once confirmed)
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

// Hex digits only, lowercase: "D4:2A:E7", "d4-2a-e7" and "D42AE7" all
// become "d42ae7". MAC and OUI filters are matched in this form, because
// filters arrive in either spelling: the setup portal stores "00:25:df",
// a mesh push or live add stores bare hex "0025DF" (the wire format), and
// NimBLE reports "d4:2a:e7:86:97:42". Comparing the raw strings meant a
// mesh-pushed MAC or OUI filter could never match a BLE device.
static String macHex(const String& mac) {
    String out;
    out.reserve(12);
    for (unsigned i = 0; i < mac.length(); i++) {
        char c = mac[i];
        if (c >= 'A' && c <= 'F') c = c - 'A' + 'a';
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) out += c;
    }
    return out;
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

// Enqueue a detection report. Producers: the BLE callback (NimBLE host
// task), the WiFi promiscuous callback (driver context: fixed buffers only, no
// heap) and the mesh RX drain. The slot copy and the head/count update happen
// inside one critical section, concurrently with the loop() drain. Drops the
// entry if the ring is full.
static void detQueuePush(const char* mac, const char* ident, const char* desc, int rssi,
                         uint8_t type, bool wifi, bool update, const uint8_t* info, uint8_t infoLen) {
    DetectionEntry e;
    strlcpy(e.mac, mac, sizeof(e.mac));
    strlcpy(e.identifier, ident, sizeof(e.identifier));
    strlcpy(e.description, desc, sizeof(e.description));
    e.rssi = rssi;
    e.matchedType = (FilterType)type;
    e.wifiDomain = wifi;
    e.update = update;
    e.infoLen = (info && infoLen) ? (infoLen > DEVINFO_MAX ? DEVINFO_MAX : infoLen) : 0;
    if (e.infoLen) memcpy(e.info, info, e.infoLen);
    portENTER_CRITICAL(&detQueueMux);
    if (detQueueCount < DET_QUEUE_SIZE) {
        detQueue[detQueueHead] = e;
        detQueueHead = (detQueueHead + 1) % DET_QUEUE_SIZE;
        detQueueCount++;
    }
    portEXIT_CRITICAL(&detQueueMux);
}

void enqueueDetection(const char* mac, const char* ident, const char* desc,
                      int rssi, FilterType type, bool wifi, const DevInfo* info) {
    detQueuePush(mac, ident, desc, rssi, (uint8_t)type, wifi, false,
                 info ? info->b : nullptr, info ? info->n : 0);
}

// Bridge for mesh.cpp: a detection that arrived over the node mesh lands in
// the exact same ring as a local one, so alerts, device history, and (on the
// home node) MQTT all treat it uniformly. `type` is the FilterType as uint8.
void meshReport(const char* mac, const char* ident, const char* desc,
                int rssi, uint8_t type, bool wifi, bool update,
                const uint8_t* info, uint8_t infoLen) {
    detQueuePush(mac, ident, desc, rssi, type, wifi, update, info, infoLen);
}

// ================================
// Mesh remote management (executed on remote nodes)
// ================================
// Binary filter record (wire format): u8 type, u8 flags (bit0 = full MAC),
// u8 idLen, u8 descLen, id[idLen], desc[descLen]. Pushed lists, adds and
// removes are applied to the running list (filtersEditLive) and saved.
// Validate one filter record list (MESH_CMD_FILTERS wire format) without
// touching NVS. Identifier rules follow the filter type: OUIs and WiFi
// signatures are 6 hex, full MACs 12 hex, company IDs / 16-bit service
// UUIDs 4 hex, name substrings and the Meta composite 1-16 printable chars.
static bool filterRecordsValid(const uint8_t* buf, uint16_t len, uint8_t& countOut) {
    uint16_t off = 0;
    uint8_t count = 0;
    while (off < len) {
        if (off + 4 > len || count >= FILTER_MAX) return false;
        uint8_t type = buf[off], idLen = buf[off + 2], descLen = buf[off + 3];
        off += 4;
        if (type > FT_WIFI_BEACON || idLen == 0 || idLen > 16 || descLen > 48 ||
            off + idLen + descLen > len) return false;
        const char* id = (const char*)(buf + off);
        uint8_t needHex = 0;
        switch ((FilterType)type) {
            case FT_MAC_PREFIX: case FT_WIFI_PROBE: case FT_WIFI_BEACON: needHex = 6; break;
            case FT_FULL_MAC: needHex = 12; break;
            case FT_COMPANY_ID: case FT_SERVICE_UUID_16: needHex = 4; break;
            default: break;
        }
        for (uint8_t i = 0; i < idLen; i++) {
            unsigned char c = (unsigned char)id[i];
            if (needHex ? !isxdigit(c) : (c < 0x20 || c > 0x7E)) return false;
        }
        if (needHex && idLen != needHex) return false;
        for (uint8_t i = 0; i < descLen; i++) {
            if ((unsigned char)buf[off + idLen + i] < 0x20) return false;
        }
        off += idLen + descLen;
        count++;
    }
    countOut = count;
    return count > 0;
}

// Live filter edits: a pushed list replaces the running one, an add merges
// into it, a remove takes entries out. The new list is built on the side
// and swapped in under the read guard, then saved; the node keeps scanning.
// The one exception: a node that booted with no BLE filters never started
// its BLE scanner, so its first BLE filter needs a restart (see
// liveEditNeedsRestart).
enum LiveFilterOp { LIVE_REPLACE, LIVE_ADD, LIVE_REMOVE };

static TargetFilter filterFromRecord(const uint8_t* buf, uint16_t& off) {
    TargetFilter f;
    f.type = (FilterType)buf[off];
    f.isFullMAC = (buf[off + 1] & 0x01) != 0;
    uint8_t idLen = buf[off + 2], descLen = buf[off + 3];
    off += 4;
    char ident[17], desc[49];
    memcpy(ident, buf + off, idLen);
    ident[idLen] = '\0';
    memcpy(desc, buf + off + idLen, descLen);
    desc[descLen] = '\0';
    off += idLen + descLen;
    f.identifier = ident;
    f.description = desc;
    return f;
}

// Returns the installed count after the edit, or -1 when refused: records
// this firmware can't take, or an edit that would leave nothing to scan for.
// `changed` counts entries added, removed or re-described.
static int filtersEditLive(LiveFilterOp op, const uint8_t* buf, uint16_t len, int& changed) {
    changed = 0;
    uint8_t total = 0;
    if (!filterRecordsValid(buf, len, total)) return -1;
    std::vector<TargetFilter> next;
    if (op != LIVE_REPLACE) next = targetFilters;   // loop is the only writer: a plain copy is safe
    uint16_t off = 0;
    for (uint8_t i = 0; i < total; i++) {
        TargetFilter r = filterFromRecord(buf, off);
        auto it = std::find_if(next.begin(), next.end(), [&](const TargetFilter& f) {
            return f.type == r.type && f.identifier.equalsIgnoreCase(r.identifier);
        });
        if (op == LIVE_REMOVE) {
            if (it != next.end()) { next.erase(it); changed++; }
        } else if (it != next.end()) {
            if (r.description.length() && it->description != r.description) {
                it->description = r.description;
                changed++;
            }
        } else if (next.size() < FILTER_MAX) {
            next.push_back(r);
            changed++;
        }
    }
    if (next.empty()) return -1;
    filterEditBegin();
    targetFilters.swap(next);
    filterEditEnd();
    saveConfiguration();
    return (int)targetFilters.size();
}

// True when the running scanner can't see part of the live list: scanning
// without a BLE scanner (none was started at boot) while BLE filters exist.
// Set when scanning started with no filters (a mesh-only node waiting for
// its list): neither scanner was set up, so the first list restarts it.
static bool g_startedEmpty = false;

static bool liveEditNeedsRestart() {
    if (currentMode == SCANNING_MODE && g_startedEmpty && !targetFilters.empty()) return true;
    if (currentMode != SCANNING_MODE || pBLEScan) return false;
    for (const TargetFilter& f : targetFilters) {
        if (!isWifiDomain(f.type)) return true;
    }
    return false;
}

// Ack first (flushed so it leaves the radio), then restart.
static void restartAfterAck(const char* why) {
    for (int i = 0; i < 30 && meshTxPending() > 0; i++) {
        meshFlushTxNow();
        delay(100);
    }
    if (isSerialConnected()) {
        Serial.printf("[MESH] %s — restarting to start the BLE scanner\n", why);
        Serial.flush();
    }
    mesh_deinit();
    delay(200);
    ESP.restart();
}

// Remote management state + helpers (definitions below meshApplyCommand).
static uint32_t statusDueAt = 0;
static uint8_t statusCopies = 0;   // STATUS goes out 3 times (radios time-slice)
static uint32_t filtersReportDueAt = 0;
static uint32_t identifyUntil = 0, identifyNextAt = 0;
static uint8_t identifyFlags = 0;
static const char* PRESET_NAMES[] = { "axon", "meta", "ipro", "verkada" };
static String jsonEsc(const String& in);
static void printFilterReport(const char* id, const uint8_t* buf, uint16_t len);

static void selfIdBytes(uint8_t out[3]) {
    char hex[7];
    meshSelfIdHex(hex);
    for (int i = 0; i < 3; i++) {
        char b[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        out[i] = (uint8_t)strtol(b, nullptr, 16);
    }
}

static bool presetLookup(uint8_t idx, const PresetEntry*& table, size_t& count, const char*& label) {
    switch (idx) {
        case 0: table = PRESET_AXON;    count = PRESET_AXON_COUNT;    label = "Axon body cam";   return true;
        case 1: table = PRESET_META;    count = PRESET_META_COUNT;    label = "META / RAY-BAN";  return true;
        case 2: table = PRESET_IPRO;    count = PRESET_IPRO_COUNT;    label = "I-PRO body cam";  return true;
        case 3: table = PRESET_VERKADA; count = PRESET_VERKADA_COUNT; label = "Verkada camera";  return true;
    }
    return false;
}

// Command handler invoked by meshTick (loop context) with a complete,
// CRC-verified payload. Acks before any reboot-triggering action, and the
// TX queue is flushed so the ack actually leaves the radio.
void meshApplyCommand(uint8_t cmd, uint16_t cmdId, const uint8_t* payload, uint16_t len) {
    switch (cmd) {
    case MESH_CMD_AUDIO: {
        if (len < 2) { meshSendAck(cmd, cmdId, 1, 0); return; }
        bool b = payload[0] != 0;
        bool l = payload[1] != 0;
        bool changed = (buzzerEnabled != b) || (ledEnabled != l);
        buzzerEnabled = b;
        ledEnabled = l;
        saveConfiguration();   // mute survives reboots
        if (isSerialConnected()) {
            Serial.printf("[MESH] audio %s by home (buzzer %s, LED %s)\n",
                          changed ? "changed" : "confirmed",
                          buzzerEnabled ? "on" : "MUTED", ledEnabled ? "on" : "off");
        }
        meshSendAck(cmd, cmdId, 0, 0);
        break;
    }
    case MESH_CMD_FILTERS:
    case MESH_CMD_FILTER_ADD:
    case MESH_CMD_FILTER_DEL: {
        LiveFilterOp op = cmd == MESH_CMD_FILTERS ? LIVE_REPLACE :
                          cmd == MESH_CMD_FILTER_ADD ? LIVE_ADD : LIVE_REMOVE;
        static const char* const OP_NAMES[] = { "replaced", "added", "removed" };
        int changed = 0;
        int n = filtersEditLive(op, payload, len, changed);
        meshSendAck(cmd, cmdId, n < 0 ? 1 : 0, n < 0 ? 0 : (uint8_t)n);
        if (n < 0) {
            if (isSerialConnected()) {
                Serial.println("[MESH] filter edit from home refused (invalid, or it would leave no filters)");
            }
            break;
        }
        if (isSerialConnected()) {
            Serial.printf("[MESH] filters %s live by home: %d changed, %d installed\n",
                          OP_NAMES[op], changed, n);
        }
        if (liveEditNeedsRestart()) restartAfterAck("first BLE filter installed");
        break;
    }
    case MESH_CMD_REBOOT:
        meshSendAck(cmd, cmdId, 0, 0);
        for (int i = 0; i < 30 && meshTxPending() > 0; i++) {
            meshFlushTxNow();
            delay(100);
        }
        if (isSerialConnected()) {
            Serial.println("[MESH] reboot commanded by home");
            Serial.flush();
        }
        mesh_deinit();   // says goodbye to the mesh
        delay(200);
        ESP.restart();
        break;
    case MESH_CMD_SETCHAN: {
        // Re-channel the whole mesh: save the new channel, ack, say
        // goodbye, reboot. Locked config is untouched — the node comes
        // back up on the new channel still burned in.
        if (len < 1 || payload[0] < 1 || payload[0] > 11) {
            meshSendAck(cmd, cmdId, 1, 0);
            return;
        }
        meshCfg.channel = payload[0];
        mesh_saveConfig();
        meshSendAck(cmd, cmdId, 0, meshCfg.channel);
        for (int i = 0; i < 30 && meshTxPending() > 0; i++) {
            meshFlushTxNow();
            delay(100);
        }
        if (isSerialConnected()) {
            Serial.printf("[MESH] re-channeled to %d by home — rebooting\n", meshCfg.channel);
            Serial.flush();
        }
        mesh_deinit();
        delay(200);
        ESP.restart();
        break;
    }
    case MESH_CMD_TARGET: {
        if (len < 4) { meshSendAck(cmd, cmdId, 1, 0); return; }
        uint8_t me[3];
        selfIdBytes(me);
        bool all = !payload[0] && !payload[1] && !payload[2];
        if (!all && memcmp(payload, me, 3) != 0) return;   // not for us: stay quiet
        uint8_t inner = payload[3];
        // Re-channeling one node would split it off the fleet; reports and
        // nested wrappers make no sense addressed.
        if (inner == MESH_CMD_TARGET || inner == MESH_CMD_SETCHAN ||
            inner == MESH_CMD_FILTERS_REPORT) {
            meshSendAck(inner, cmdId, 1, 0);
            return;
        }
        meshApplyCommand(inner, cmdId, payload + 4, len - 4);
        break;
    }
    case MESH_CMD_STATUS:
        // The STATUS packet is the reply. Jittered so a fleet-wide request
        // doesn't make every node transmit in the same instant.
        statusDueAt = millis() + random(150, 2500);
        if (!statusDueAt) statusDueAt = 1;
        statusCopies = 3;
        meshHoldRadio(5500);   // stay on the mesh until all three copies are out
        if (isSerialConnected()) Serial.println("[MESH] status requested by home, replying");
        break;
    case MESH_CMD_IDENTIFY: {
        uint8_t secs = len >= 1 ? payload[0] : 10;
        if (secs < 1) secs = 1;
        if (secs > 120) secs = 120;
        identifyFlags = len >= 2 ? payload[1] : 3;
        identifyUntil = millis() + secs * 1000UL;
        identifyNextAt = 0;
        if (isSerialConnected()) Serial.printf("[MESH] identify for %u s (home asked)\n", secs);
        meshSendAck(cmd, cmdId, 0, secs);
        break;
    }
    case MESH_CMD_PRESET: {
        const PresetEntry* table = nullptr;
        size_t count = 0;
        const char* label = nullptr;
        if (len < 1 || !presetLookup(payload[0], table, count, label)) {
            meshSendAck(cmd, cmdId, 1, 0);
            return;
        }
        filterEditBegin();
        int added = applyPreset(table, count, label);   // saves when it adds
        filterEditEnd();
        meshSendAck(cmd, cmdId, 0, (uint8_t)added);
        if (isSerialConnected()) {
            Serial.printf("[MESH] preset %s installed live by home (%d new)\n", PRESET_NAMES[payload[0]], added);
        }
        if (liveEditNeedsRestart()) restartAfterAck("first BLE filter installed");
        break;
    }
    case MESH_CMD_ALIAS: {
        // The dashboard named this node: keep it, so every view (its own
        // web UI, a display on its USB, the status it floods) agrees.
        String a;
        for (uint16_t i = 0; i < len && i < ALIAS_MAX; i++) a += (char)payload[i];
        if (!aliasValid(a)) { meshSendAck(cmd, cmdId, 1, 0); return; }
        Preferences p;
        p.begin("ouispy", false);
        p.putString("alias", a);
        p.end();
        meshSendAck(cmd, cmdId, 0, 0);
        if (isSerialConnected()) Serial.printf("[MESH] alias set by home: %s\n", a.length() ? a.c_str() : "(cleared)");
        break;
    }
    case MESH_CMD_WARDRIVE: {
        if (!wdApplyMesh(payload, len)) { meshSendAck(cmd, cmdId, 1, 0); return; }   // not a wardriver
        meshSendAck(cmd, cmdId, 0, 0);
        if (isSerialConnected()) Serial.println("[MESH] wardrive settings from the dashboard");
        break;
    }
    case MESH_CMD_GETFILTERS:
        meshHoldRadio(4000);
        filtersReportDueAt = millis() + random(200, 3000);
        if (!filtersReportDueAt) filtersReportDueAt = 1;
        break;
    case MESH_CMD_OTA_OFFER: {
        // A firmware update offer (signed, addressed). Accepting it starts
        // the receiver; the transfer itself runs in mesh_ota.cpp.
        bool ok = meshOtaOffer(payload, len);
        meshSendAck(cmd, cmdId, ok ? 0 : 1, 0);
        if (!ok && isSerialConnected()) Serial.println("[OTA] update offer refused (no fleet key, no OTA partition, or too big)");
        break;
    }
    case MESH_CMD_FILTERS_REPORT: {
        // {reporterId[3], records...}: print it, never apply or ack it.
        if (len < 3) return;
        char hex[7];
        snprintf(hex, sizeof(hex), "%02X%02X%02X", payload[0], payload[1], payload[2]);
        printFilterReport(hex, payload + 3, len - 3);
        break;
    }
    default:
        meshSendAck(cmd, cmdId, 1, 0);
        break;
    }
}

// ================================
// Remote management (dashboard <-> home <-> fleet)
// ================================
// The home dashboard drives the fleet through the USB console. The home
// floods the commands, and every reply comes back as one machine-readable
// line (human logs are untouched):
//   RST {json}  a node's settings + health (a STATUS reply, or "status" locally)
//   ACK {json}  a command ack from a node, with its stable ID
//   RFL {json}  a node's installed filter list (a FILTERS_REPORT, or "filters")
//   CMD {json}  a console command was accepted and flooded; id = cmdId
#define STATUS_FMT 1
#define STATUS_LEN_BASE 52   // first build; still accepted from older nodes
#define STATUS_LEN 56        // + u8 keyFingerprint[4]
// + u8 count, up to 8 neighbors x 5 bytes, then u8 alias length + alias
#define STATUS_MAX (STATUS_LEN + 1 + 8 * 5 + 1 + ALIAS_MAX)
static String nodeAlias();
static const char FW_BUILD[] = __DATE__ " " __TIME__;

// Key fingerprint: SHA-256("OUISPY-KEY-FP" | fleet key)[0:4], all zeros
// with no key. One-way and only 32 bits, so it can't recover the key, but
// it tells the dashboard whether every node holds the SAME key.
static void keyFingerprint(uint8_t out[4]) {
    memset(out, 0, 4);
    if (!meshKeySet()) return;
    uint8_t h[32];
    mbedtls_sha256_context sh;
    mbedtls_sha256_init(&sh);
    mbedtls_sha256_starts(&sh, 0);
    mbedtls_sha256_update(&sh, (const unsigned char*)"OUISPY-KEY-FP", 13);
    mbedtls_sha256_update(&sh, (const unsigned char*)meshCfg.key, strlen(meshCfg.key));
    mbedtls_sha256_finish(&sh, h);
    mbedtls_sha256_free(&sh);
    memcpy(out, h, 4);
}

static void put16(uint8_t* d, uint8_t& n, uint16_t v) { memcpy(d + n, &v, 2); n += 2; }
static void put32(uint8_t* d, uint8_t& n, uint32_t v) { memcpy(d + n, &v, 4); n += 4; }
static uint16_t get16(const uint8_t* d) { uint16_t v; memcpy(&v, d, 2); return v; }
static uint32_t get32(const uint8_t* d) { uint32_t v; memcpy(&v, d, 4); return v; }

static uint16_t serializeFilters(uint8_t* buf, uint16_t cap, bool& tooBig, bool withDesc) {
    uint16_t n = 0;
    tooBig = false;
    for (const TargetFilter& f : targetFilters) {
        uint8_t idLen = f.identifier.length();
        uint8_t descLen = withDesc ? min((int)f.description.length(), 48) : 0;
        if (idLen == 0 || idLen > 16) continue;
        if (n + 4 + idLen + descLen > cap) { tooBig = true; break; }
        buf[n++] = (uint8_t)f.type;
        buf[n++] = f.isFullMAC ? 1 : 0;
        buf[n++] = idLen;
        buf[n++] = descLen;
        memcpy(buf + n, f.identifier.c_str(), idLen);    n += idLen;
        memcpy(buf + n, f.description.c_str(), descLen); n += descLen;
    }
    return n;
}

// CRC32 of the serialized list: equal CRCs = identical filter sets, so the
// dashboard can tell at a glance which nodes drifted from the home.
// Computed record by record over the WHOLE list (a 4 KB buffer used to cut
// long lists short, so the CRC stopped matching the dashboard's).
static uint32_t filtersCrc() {
    uint32_t crc = 0;
    uint8_t rec[4 + 16 + 48];
    for (const TargetFilter& f : targetFilters) {
        uint8_t idLen = f.identifier.length();
        uint8_t descLen = min((int)f.description.length(), 48);
        if (idLen == 0 || idLen > 16) continue;
        rec[0] = (uint8_t)f.type;
        rec[1] = f.isFullMAC ? 1 : 0;
        rec[2] = idLen;
        rec[3] = descLen;
        memcpy(rec + 4, f.identifier.c_str(), idLen);
        memcpy(rec + 4 + idLen, f.description.c_str(), descLen);
        crc = crc32_le(crc, rec, 4 + idLen + descLen);
    }
    return crc;
}

static uint8_t buildStatus(uint8_t* d) {
    uint32_t sent = 0, retries = 0, acked = 0, expired = 0;
    uint8_t pend = 0;
    if (meshCfg.enabled) meshDeliveryStats(sent, retries, acked, expired, pend);
    Preferences p;
    p.begin("ouispy", true);
    bool locked = p.getBool("configLocked", false);
    p.end();
    uint8_t n = 0;
    d[n++] = STATUS_FMT;
    d[n++] = meshCfg.home ? 1 : 0;
    d[n++] = currentMode == SCANNING_MODE ? 1 : 0;
    d[n++] = meshCfg.channel;
    d[n++] = (buzzerEnabled ? 1 : 0) | (ledEnabled ? 2 : 0) | (meshKeySet() ? 4 : 0) |
             (locked ? 8 : 0) | (mqttCfg.enabled ? 16 : 0) |
             ((meshCfg.enabled && meshHealing()) ? 32 : 0);
    d[n++] = meshCfg.enabled ? meshNodeCount() : 0;
    d[n++] = (uint8_t)(meshCfg.enabled ? meshLinkRssi() : 0);
    d[n++] = 0;
    put16(d, n, (uint16_t)targetFilters.size());
    put32(d, n, filtersCrc());
    put32(d, n, millis() / 1000);
    put32(d, n, ESP.getFreeHeap());
    put16(d, n, (uint16_t)devices.size());
    put32(d, n, sent);
    put32(d, n, acked);
    put32(d, n, expired);
    memset(d + n, 0, 16);
    // Compact build stamp, "Oct27 21:21:25": the full "Oct 27 2026 21:21:25"
    // was cut to 15 chars, so every build within an hour looked the same.
    {
        // From FW_BUILD itself ("Oct 27 2026 21:21:25", day space-padded
        // below 10), which also keeps that full stamp in the image, where
        // the dashboard reads an uploaded build's version.
        const char* b = FW_BUILD;
        char* o = (char*)d + n;
        o[0] = b[0]; o[1] = b[1]; o[2] = b[2];
        o[3] = b[4] == ' ' ? '0' : b[4]; o[4] = b[5];
        o[5] = ' ';
        memcpy(o + 6, b + 12, 8);
#ifdef SKYSPY_NODE
        o[14] = 'S';    // a Sky-Spy node
#elif defined(WARDRIVE_NODE)
        o[14] = 'W';    // a wardriver
#endif
    }
    n += 16;
    keyFingerprint(d + n);   // 0000 when no key: lets the dashboard check keys match
    n += 4;
    // STATUS_LEN so far. Then the direct neighbors: u8 count, 5 bytes each
    // (older homes ignore the tail; newer dashboards draw the links).
    uint8_t cnt = meshCfg.enabled ? meshNeighbors(d + n + 1, 8) : 0;
    d[n] = cnt;
    n += 1 + cnt * 5;
    // Then the node's alias (set by the web flasher): u8 length, ASCII.
    String a = nodeAlias();
    uint8_t al = (uint8_t)min((unsigned)a.length(), (unsigned)ALIAS_MAX);
    d[n++] = al;
    memcpy(d + n, a.c_str(), al);
    n += al;
    return n;
}

// The name this node goes by on the dashboard, stored on the node itself so
// it travels with it (the web flasher sets it while provisioning).
static String nodeAlias() {
    Preferences p;
    p.begin("ouispy", true);
    String a = p.getString("alias", "");
    p.end();
    return a;
}


static void printStatusLine(const char* id, const uint8_t* d, uint8_t len) {
    if (!isSerialConnected()) return;
    if (len < STATUS_LEN_BASE || d[0] != STATUS_FMT) {
        Serial.printf("RST {\"node\":\"%s\",\"fmt\":%u}\n", id, len ? d[0] : 0);
        return;
    }
    uint8_t f = d[4];
    char fw[17];
    memcpy(fw, d + 36, 16);
    fw[16] = '\0';
    for (int i = 0; fw[i]; i++) if (fw[i] == '"' || fw[i] == '\\' || fw[i] < 0x20) fw[i] = ' ';
    char kfp[24] = "";   // key fingerprint, from nodes that send it
    if (len >= STATUS_LEN) {
        snprintf(kfp, sizeof(kfp), ",\"kfp\":\"%02X%02X%02X%02X\"", d[52], d[53], d[54], d[55]);
    }
    // Direct neighbors, from nodes that send them: "nbr":[["1A2B3C",-61,2],...]
    char nbr[8 * 22 + 12] = "";
    if (len > STATUS_LEN) {
        uint8_t cnt = d[STATUS_LEN];
        if (cnt > 8 || len < STATUS_LEN + 1 + cnt * 5) cnt = 0;
        int w = snprintf(nbr, sizeof(nbr), ",\"nbr\":[");
        for (uint8_t i = 0; i < cnt; i++) {
            const uint8_t* e = d + STATUS_LEN + 1 + i * 5;
            w += snprintf(nbr + w, sizeof(nbr) - w, "%s[\"%02X%02X%02X\",%d,%u]", i ? "," : "",
                          e[0], e[1], e[2], (int)(int8_t)e[3], e[4]);
        }
        snprintf(nbr + w, sizeof(nbr) - w, "]");
    }
    // The node's alias, after the neighbors: "alias":"North Gate"
    String aliasJson;
    if (len > STATUS_LEN && d[STATUS_LEN] <= 8) {
        uint16_t at = STATUS_LEN + 1 + d[STATUS_LEN] * 5;
        if (at < len && d[at] <= ALIAS_MAX && at + 1 + d[at] <= len) {
            char tmp[ALIAS_MAX + 1];
            memcpy(tmp, d + at + 1, d[at]);
            tmp[d[at]] = '\0';
            aliasJson = ",\"alias\":\"" + jsonEsc(String(tmp)) + "\"";
        }
    }
    Serial.printf("RST {\"node\":\"%s\",\"role\":\"%s\",\"mode\":\"%s\",\"ch\":%u,"
                  "\"buzzer\":%d,\"led\":%d,\"key\":%d,\"locked\":%d,\"mqtt\":%d,\"heal\":%d,"
                  "\"peers\":%u,\"lrssi\":%d,\"filters\":%u,\"fcrc\":\"%08lX\",\"up\":%lu,"
                  "\"heap\":%lu,\"dev\":%u,\"dsent\":%lu,\"dacked\":%lu,\"dexp\":%lu,\"fw\":\"%s\"%s%s%s}\n",
                  id, d[1] ? "home" : "remote", d[2] ? "scan" : "config", d[3],
                  (f & 1) ? 1 : 0, (f & 2) ? 1 : 0, (f & 4) ? 1 : 0, (f & 8) ? 1 : 0,
                  (f & 16) ? 1 : 0, (f & 32) ? 1 : 0, d[5], (int)(int8_t)d[6],
                  get16(d + 8), (unsigned long)get32(d + 10), (unsigned long)get32(d + 14),
                  (unsigned long)get32(d + 18), get16(d + 22), (unsigned long)get32(d + 24),
                  (unsigned long)get32(d + 28), (unsigned long)get32(d + 32), fw, kfp, nbr,
                  aliasJson.c_str());
}

static void printFilterReport(const char* id, const uint8_t* buf, uint16_t len) {
    if (!isSerialConnected()) return;
    uint8_t count = 0;
    bool ok = filterRecordsValid(buf, len, count) || len == 0;
    Serial.printf("RFL {\"node\":\"%s\",\"ok\":%d,\"filters\":[", id, ok ? 1 : 0);
    uint16_t off = 0;
    for (uint8_t i = 0; ok && i < count; i++) {
        uint8_t type = buf[off], idLen = buf[off + 2], descLen = buf[off + 3];
        bool full = (buf[off + 1] & 1) != 0;
        off += 4;
        String ident = String((const char*)(buf + off)).substring(0, idLen);
        char tmp[49];
        memcpy(tmp, buf + off + idLen, descLen);
        tmp[descLen] = '\0';
        off += idLen + descLen;
        Serial.printf("%s{\"t\":\"%s\",\"id\":\"%s\",\"d\":\"%s\",\"full\":%d}",
                      i ? "," : "", filterTypeCode((FilterType)type), jsonEsc(ident).c_str(),
                      jsonEsc(String(tmp)).c_str(), full ? 1 : 0);
    }
    Serial.println("]}");
}

void meshOnStatus(const uint8_t* nodeId, const uint8_t* data, uint8_t len) {
    char hex[7];
    snprintf(hex, sizeof(hex), "%02X%02X%02X", nodeId[0], nodeId[1], nodeId[2]);
    printStatusLine(hex, data, len);
}

void meshOnAck(const uint8_t* nodeId, uint8_t cmd, uint16_t cmdId, uint8_t status, uint8_t count) {
    if (!isSerialConnected()) return;
    Serial.printf("ACK {\"node\":\"%02X%02X%02X\",\"cmd\":%u,\"id\":%u,\"st\":%u,\"n\":%u}\n",
                  nodeId[0], nodeId[1], nodeId[2], cmd, cmdId, status, count);
}

// loop()-context: deferred replies and the identify pattern. Called next to
// every meshTick().
static void remoteMgmtTick() {
    uint32_t now = millis();
    // Live topology for the dashboard's mesh map: every ~20 s a remote
    // floods its status (with its direct neighbors) and the home prints its
    // own on USB, so link changes, and the mesh healing around them, show
    // within about half a minute.
    static uint32_t topoAt = 15000;
    if (meshCfg.enabled && currentMode == SCANNING_MODE && !meshOtaBusy() && (int32_t)(now - topoAt) >= 0) {
        topoAt = now + 20000 + random(0, 5000);
        if (meshCfg.home) {
            if (isSerialConnected()) {
                uint8_t d[STATUS_MAX];
                char id[7];
                meshSelfIdHex(id);
                printStatusLine(id, d, buildStatus(d));
            }
        } else if (!statusDueAt) {
            statusDueAt = now;
            statusCopies = 0;   // one copy: the next round is only 20 s away
        }
    }
    if (statusDueAt && (int32_t)(now - statusDueAt) >= 0) {
        uint8_t d[STATUS_MAX];
        meshSendStatus(d, buildStatus(d));
        // Repeat so it meets the home's mesh window; copies are harmless
        // (the dashboard just overwrites the same node's settings).
        statusDueAt = (statusCopies > 1) ? now + 600 + random(0, 400) : 0;
        if (statusCopies) statusCopies--;
    }
    if (filtersReportDueAt && (int32_t)(now - filtersReportDueAt) >= 0) {
        uint8_t* buf = (uint8_t*)malloc(4096);
        if (buf) {
            selfIdBytes(buf);
            bool tooBig;
            uint16_t n = 3 + serializeFilters(buf + 3, 4093, tooBig);
            // A long list doesn't fit one mesh transfer with its descriptions:
            // send type + identifier only (the dashboard knows the names).
            if (tooBig) n = 3 + serializeFilters(buf + 3, 4093, tooBig, false);
            // Busy with another transfer: try again in a second, for up to
            // 30 s (a list too big to send at all never gets through).
            static uint8_t tries = 0;
            if (meshQueueCommand(MESH_CMD_FILTERS_REPORT, meshNewCmdId(), buf, n) || ++tries >= 30) {
                filtersReportDueAt = 0;
                tries = 0;
            } else {
                filtersReportDueAt = now + 1000;
            }
            free(buf);
        }
    }
    if (identifyUntil) {
        if ((int32_t)(now - identifyUntil) >= 0) {
            identifyUntil = 0;
            ledcWrite(0, 0);
            digitalWrite(LED_PIN, HIGH);   // inverted: HIGH = off
        } else if ((int32_t)(now - identifyNextAt) >= 0) {
            identifyNextAt = now + 900;
            // Deliberately ignores mute/LED settings: the point is finding it.
            startDetectionFlash();
            if (identifyFlags & 2) digitalWrite(LED_PIN, LOW);
            if (identifyFlags & 1) {
                ledcSetup(0, BUZZER_FREQ, 8);
                ledcWrite(0, BUZZER_DUTY);
            }
            delay(90);
            ledcWrite(0, 0);
            digitalWrite(LED_PIN, HIGH);
        }
    }
}

static bool parseTarget(String t, uint8_t out[3], bool& all) {
    t.trim();
    t.toUpperCase();
    if (t == "ALL" || t == "*") { memset(out, 0, 3); all = true; return true; }
    if (t.length() != 6) return false;
    for (int i = 0; i < 6; i++) if (!isxdigit((unsigned char)t[i])) return false;
    for (int i = 0; i < 3; i++) out[i] = (uint8_t)strtol(t.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
    all = false;
    return true;
}

// Flood a management command: fleet-wide commands go out unwrapped (so
// older firmware still obeys AUDIO/REBOOT/FILTERS), addressed ones inside a
// MESH_CMD_TARGET wrapper.
static bool sendMgmt(const char* op, const uint8_t t[3], bool all, uint8_t cmd,
                     const uint8_t* p, uint16_t len) {
    if (!meshCfg.enabled) { Serial.printf("ERR %s: mesh disabled on this node\n", op); return false; }
    uint16_t id = meshNewCmdId();
    bool ok;
    if (all) {
        ok = meshQueueCommand(cmd, id, p, len);
    } else {
        uint8_t* buf = (uint8_t*)malloc(len + 4);
        if (!buf) { Serial.printf("ERR %s: out of memory\n", op); return false; }
        memcpy(buf, t, 3);
        buf[3] = cmd;
        if (len) memcpy(buf + 4, p, len);
        ok = meshQueueCommand(MESH_CMD_TARGET, id, buf, len + 4);
        free(buf);
    }
    if (!ok) { Serial.printf("ERR %s: mesh busy (transfer in progress) or down\n", op); return false; }
    char hex[7] = "all";
    if (!all) snprintf(hex, sizeof(hex), "%02X%02X%02X", t[0], t[1], t[2]);
    Serial.printf("CMD {\"op\":\"%s\",\"id\":%u,\"target\":\"%s\",\"cmd\":%u}\n", op, id, hex, cmd);
    return true;
}

// Staged filter upload from the dashboard: mfbegin <target|self|all>
// (replace the list), mfadd (merge into it) or mfdel (remove from it), then
// mfdata <hex> lines (short enough for the USB RX buffer, each answered
// "OK mfdata <bytes>" so the sender paces itself), then mfend. Every one
// applies live on the node: no reboot.
static uint8_t* mfBuf = nullptr;
static uint16_t mfLen = 0;
static uint8_t mfTarget[3];
static bool mfAll = false, mfSelf = false;
static LiveFilterOp mfOp = LIVE_REPLACE;   // mfbegin = replace, mfadd = merge, mfdel = remove

static void localReboot(const char* why) {
    Serial.printf("OK %s — rebooting\n", why);
    Serial.flush();
    for (int i = 0; i < 20 && meshTxPending() > 0; i++) { meshFlushTxNow(); delay(100); }
    mesh_deinit();
    delay(200);
    ESP.restart();
}

// Returns true when the line was one of ours (handled, even if it failed).
static bool remoteConsoleCommand(const String& line) {
    int sp = line.indexOf(' ');
    String verb = sp < 0 ? line : line.substring(0, sp);
    String rest = sp < 0 ? String("") : line.substring(sp + 1);
    rest.trim();
    String arg[4];
    {
        String r = rest;
        for (int i = 0; i < 4; i++) {
            r.trim();
            int s2 = r.indexOf(' ');
            arg[i] = s2 < 0 ? r : r.substring(0, s2);
            r = s2 < 0 ? String("") : r.substring(s2 + 1);
        }
    }
    uint8_t t[3];
    bool all = false;
    char selfHex[7];
    meshSelfIdHex(selfHex);

    if (verb == "status") {
        uint8_t d[STATUS_MAX];
        printStatusLine(selfHex, d, buildStatus(d));
        return true;
    }
    if (verb == "filters") {
        const uint16_t cap = FILTER_MAX * (4 + 16 + 48);
        uint8_t* buf = (uint8_t*)malloc(cap);
        if (!buf) { Serial.println("ERR out of memory"); return true; }
        bool tooBig;
        uint16_t n = serializeFilters(buf, cap, tooBig);
        printFilterReport(selfHex, buf, n);
        free(buf);
        return true;
    }
    if (verb == "audio") {          // local: audio <buzzer 0|1> <led 0|1>
        if (arg[0].length() == 0 || arg[1].length() == 0) { Serial.println("ERR usage: audio <0|1> <0|1>"); return true; }
        buzzerEnabled = arg[0].toInt() != 0;
        ledEnabled = arg[1].toInt() != 0;
        saveConfiguration();
        Serial.printf("OK audio buzzer=%d led=%d\n", buzzerEnabled, ledEnabled);
        return true;
    }
    if (wdConsole(verb, rest)) return true;   // wd, wd on|off, wd raw on|off, wd ch ...
    if (verb == "rid") {            // rid on|off: drone Remote ID (keeps WiFi + BLE scanning)
#ifdef SKYSPY_NODE
        Serial.println("OK rid on (always on in a Sky-Spy node)");
        return true;
#endif
        if (arg[0] == "on" || arg[0] == "off") {
            ridEnabled = arg[0] == "on";
            Preferences p;
            p.begin("ouispy", false);
            p.putBool("rid", ridEnabled);
            p.end();
            Serial.printf("OK rid %s (restarting to apply)\n", ridEnabled ? "on" : "off");
            Serial.flush();
            localReboot("rid");
        } else {
            Serial.printf("OK rid %s\n", ridEnabled ? "on" : "off");
        }
        return true;
    }
    if (verb == "nodealias") {      // nodealias <name> | nodealias - (clear)
        String a = rest == "-" ? String("") : rest;
        if (!aliasValid(a)) { Serial.println("ERR nodealias: up to 32 plain ASCII characters, no quotes or backslashes"); return true; }
        Preferences p;
        p.begin("ouispy", false);
        p.putString("alias", a);
        p.end();
        Serial.printf("OK nodealias %s\n", a.length() ? a.c_str() : "(cleared)");
        Serial.flush();
        return true;
    }
    if (verb == "reboot") { localReboot("reboot"); return true; }
    if (verb == "ident") {          // local identify: ident [secs]
        int secs = arg[0].length() ? arg[0].toInt() : 10;
        identifyFlags = 3;
        identifyUntil = millis() + (uint32_t)constrain(secs, 1, 120) * 1000UL;
        identifyNextAt = 0;
        Serial.printf("OK identify %d s\n", constrain(secs, 1, 120));
        return true;
    }

    bool remote = verb == "rstatus" || verb == "raudio" || verb == "rreboot" ||
                  verb == "rident" || verb == "rpreset" || verb == "rfilters" || verb == "ralias" || verb == "rwd";
    if (remote) {
        if (!parseTarget(arg[0], t, all)) { Serial.printf("ERR %s: target must be a node ID or all\n", verb.c_str()); return true; }
        if (verb == "rstatus") {
            sendMgmt("rstatus", t, all, MESH_CMD_STATUS, nullptr, 0);
        } else if (verb == "rfilters") {
            sendMgmt("rfilters", t, all, MESH_CMD_GETFILTERS, nullptr, 0);
            // The report is multi-chunk and trails the request by up to ~7 s:
            // stay on the mesh channel to catch it instead of sweeping.
            meshHoldRadio(15000);
        } else if (verb == "rreboot") {
            sendMgmt("rreboot", t, all, MESH_CMD_REBOOT, nullptr, 0);
        } else if (verb == "raudio") {
            if (arg[1].length() == 0 || arg[2].length() == 0) { Serial.println("ERR usage: raudio <target> <0|1> <0|1>"); return true; }
            uint8_t p[2] = { (uint8_t)(arg[1].toInt() != 0), (uint8_t)(arg[2].toInt() != 0) };
            sendMgmt("raudio", t, all, MESH_CMD_AUDIO, p, 2);
        } else if (verb == "rident") {
            int secs = arg[1].length() ? arg[1].toInt() : 10;
            uint8_t p[2] = { (uint8_t)constrain(secs, 1, 120), (uint8_t)(arg[2].length() ? arg[2].toInt() : 3) };
            sendMgmt("rident", t, all, MESH_CMD_IDENTIFY, p, 2);
        } else if (verb == "rwd") {   // rwd <target> on|off | raw on|off | ch <plan> (wardrive nodes)
            int sp = rest.indexOf(' ');
            uint8_t p[4];
            if (sp < 0 || !wdMeshPayload(rest.substring(sp + 1), p)) {
                Serial.println("ERR usage: rwd <node|all> on|off | raw on|off | ch all|6|1-6|1,6,11");
                return true;
            }
            sendMgmt("rwd", t, all, MESH_CMD_WARDRIVE, p, 4);
        } else if (verb == "ralias") {   // ralias <target> <name> | ralias <target> - (clear)
            int sp = rest.indexOf(' ');
            String a = sp < 0 ? String("") : rest.substring(sp + 1);
            a.trim();
            if (a == "-") a = "";
            if (!aliasValid(a)) { Serial.println("ERR ralias: up to 32 plain ASCII characters, no quotes or backslashes"); return true; }
            sendMgmt("ralias", t, all, MESH_CMD_ALIAS, (const uint8_t*)a.c_str(), (uint16_t)a.length());
        } else if (verb == "rpreset") {
            int idx = -1;
            String name = arg[1];
            name.toLowerCase();
            for (int i = 0; i < 4; i++) if (name == PRESET_NAMES[i]) idx = i;
            if (idx < 0) { Serial.println("ERR unknown preset (axon|meta|ipro|verkada)"); return true; }
            uint8_t p[1] = { (uint8_t)idx };
            sendMgmt("rpreset", t, all, MESH_CMD_PRESET, p, 1);
        }
        return true;
    }

    // mfadd / mfdel are their own verbs (not an mfbegin argument) so an
    // older home answers "unknown command" instead of replacing a remote's
    // whole list with the one filter meant to be added.
    // Firmware over the mesh (mesh_ota.cpp). The dashboard stages the image
    // on this node over USB, then sends it to remotes one at a time:
    //   fwbegin <size> <sha256 hex>   fwdata <offset> <hex>   fwend
    //   fwsend <node>   fwabort   fwself (install on this node)   fwstatus
    if (verb == "fwbegin") {
        uint32_t size = (uint32_t)strtoul(arg[0].c_str(), nullptr, 10);
        uint8_t sha[32];
        if (arg[1].length() != 64) { Serial.println("ERR fwbegin: usage fwbegin <size> <sha256 hex>"); return true; }
        for (int i = 0; i < 32; i++) sha[i] = (uint8_t)strtol(arg[1].substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
        String err;
        if (!otaStageBegin(size, sha, err)) { Serial.printf("ERR fwbegin: %s\n", err.c_str()); return true; }
        Serial.println("OK fwbegin");
        return true;
    }
    if (verb == "fwdata") {
        static uint8_t buf[1024];
        const String& hex = arg[1];
        if (hex.length() % 2 || hex.length() / 2 > sizeof(buf)) { Serial.println("ERR fwdata: bad hex"); return true; }
        uint16_t n = hex.length() / 2;
        for (uint16_t i = 0; i < n; i++) {
            char h[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
            if (!isxdigit((unsigned char)h[0]) || !isxdigit((unsigned char)h[1])) { Serial.println("ERR fwdata: bad hex"); return true; }
            buf[i] = (uint8_t)strtol(h, nullptr, 16);
        }
        String err;
        uint32_t off = (uint32_t)strtoul(arg[0].c_str(), nullptr, 10);
        if (!otaStageData(off, buf, n, err)) { Serial.printf("ERR fwdata: %s\n", err.c_str()); Serial.flush(); return true; }
        Serial.printf("OK fwdata %u\n", off + n);
        Serial.flush();   // push the short reply out now, not with the next long line
        return true;
    }
    if (verb == "fwend") {
        String err;
        uint32_t size;
        if (!otaStageEnd(err)) { Serial.printf("ERR fwend: %s\n", err.c_str()); return true; }
        otaStaged(size);
        Serial.printf("OK fwend staged %u bytes\n", size);
        return true;
    }
    if (verb == "fwsend") {
        uint32_t size;
        if (!otaStaged(size)) { Serial.println("ERR fwsend: nothing staged (fwbegin/fwdata/fwend first)"); return true; }
        if (!parseTarget(arg[0], t, all) || all) { Serial.println("ERR fwsend: target must be one node ID"); return true; }
        if (meshOtaBusy()) { Serial.println("ERR fwsend: an update is already running"); return true; }
        uint8_t p[48];
        uint16_t n = otaOfferPayload(t, p);
        if (sendMgmt("fwsend", t, false, MESH_CMD_OTA_OFFER, p, n)) otaSendBegin(t, (uint16_t)(p[0] | (p[1] << 8)));
        return true;
    }
    if (verb == "fwabort") { otaSendAbort(); Serial.println("OK fwabort"); return true; }
    if (verb == "fwstatus") { otaPrintStatus(); return true; }
    if (verb == "fwself") {
        String err;
        if (!otaSelfInstall(err)) { Serial.printf("ERR fwself: %s\n", err.c_str()); return true; }
        localReboot("installing the staged firmware on this node");
        return true;
    }

    if (verb == "mfbegin" || verb == "mfadd" || verb == "mfdel") {
        String a = arg[0];
        a.toUpperCase();
        mfSelf = (a == "SELF" || a == String(selfHex));
        if (!mfSelf && !parseTarget(arg[0], mfTarget, mfAll)) { Serial.println("ERR mfbegin: target must be a node ID, self or all"); return true; }
        mfOp = verb == "mfadd" ? LIVE_ADD : verb == "mfdel" ? LIVE_REMOVE : LIVE_REPLACE;
        if (!mfBuf) mfBuf = (uint8_t*)malloc(4096);
        if (!mfBuf) { Serial.println("ERR out of memory"); return true; }
        mfLen = 0;
        Serial.println("OK mfbegin");
        return true;
    }
    if (verb == "mfdata") {
        if (!mfBuf) { Serial.println("ERR mfdata: no mfbegin"); return true; }
        if (rest.length() % 2 || mfLen + rest.length() / 2 > 4000) { Serial.println("ERR mfdata: bad hex or too long"); return true; }
        for (unsigned i = 0; i < rest.length(); i += 2) {
            if (!isxdigit((unsigned char)rest[i]) || !isxdigit((unsigned char)rest[i + 1])) { Serial.println("ERR mfdata: bad hex"); return true; }
            mfBuf[mfLen++] = (uint8_t)strtol(rest.substring(i, i + 2).c_str(), nullptr, 16);
        }
        Serial.printf("OK mfdata %u\n", mfLen);
        return true;
    }
    if (verb == "mfend") {
        if (!mfBuf) { Serial.println("ERR mfend: no mfbegin"); return true; }
        uint8_t count = 0;
        if (!filterRecordsValid(mfBuf, mfLen, count)) {
            Serial.println("ERR mfend: filter list invalid");
        } else if (mfSelf) {
            int changed = 0;
            int n = filtersEditLive(mfOp, mfBuf, mfLen, changed);
            free(mfBuf); mfBuf = nullptr;
            if (n < 0) { Serial.println("ERR mfend: refused (it would leave this node with no filters)"); return true; }
            if (liveEditNeedsRestart()) { localReboot("filters applied; restarting to start the BLE scanner"); return true; }
            Serial.printf("OK filters applied live: %d changed, %d installed\n", changed, n);
            return true;
        } else {
            static const char* const OPS[] = { "filters", "fadd", "fdel" };
            static const uint8_t CMDS[] = { MESH_CMD_FILTERS, MESH_CMD_FILTER_ADD, MESH_CMD_FILTER_DEL };
            // Mesh busy (another transfer still going out): keep the staged
            // list, so the dashboard's retry of "mfend" can send it.
            if (!sendMgmt(OPS[mfOp], mfTarget, mfAll, CMDS[mfOp], mfBuf, mfLen)) return true;
        }
        free(mfBuf);
        mfBuf = nullptr;
        return true;
    }
    return false;
}

// WiFi OUI fast-path table. A small const array of 3-byte OUIs extracted
// from installed FT_WIFI_PROBE and FT_WIFI_BEACON filters, rebuilt whenever
// those filters change. The promiscuous callback checks the frame's source OUI
// against this table before doing the full filter match — saving CPU on the
// common case (99.9% of frames don't match).
static const int WIFI_OUI_TABLE_SIZE = 256;  // max entries (16 silently dropped every WiFi OUI past the 16th)
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
        if (f.type == FT_WIFI_PROBE && (subtype == 0x04 || subtype == 0x00 || subtype == 0x02)) subtypeMatch = true;
        else if (f.type == FT_WIFI_BEACON && (subtype == 0x08 || subtype == 0x05)) subtypeMatch = true;
        else continue;

        if (subtypeMatch) {
            const char* fid = f.identifier.c_str();
            size_t flen = strlen(fid);
            // WiFi identifiers are bare 6-hex OUIs. Reject anything longer
            // (or empty) BEFORE indexing oui[] — a malformed value that
            // somehow lands in NVS must not be able to overread the 7-byte
            // stack buffer, and an empty identifier must not wildcard-match
            // every frame of the subtype.
            if (flen == 0 || flen > 6) continue;
            bool ouiMatch = true;
            for (size_t k = 0; k < flen; k++) {
                char fc = fid[k];
                char fcUp = (fc >= 'a' && fc <= 'f') ? (fc - 'a' + 'A') : fc;
                if (fcUp != oui[k]) { ouiMatch = false; break; }
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
    const String deviceHex = macHex(deviceMAC);

    for (const TargetFilter& filter : targetFilters) {
        switch (filter.type) {
            case FT_MAC_PREFIX: {
                String filterID = macHex(filter.identifier);
                if (filterID.length() >= 6 && deviceHex.startsWith(filterID)) {
                    matchedDescription = filter.description;
                    matchedIdentifier  = filter.identifier;
                    outType            = filter.type;
                    return true;
                }
                break;
            }
            case FT_FULL_MAC: {
                if (deviceHex.equals(macHex(filter.identifier))) {
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
// Ray-Ban / Oakley Meta smart glasses. Bluetooth SIG registry: company IDs
// 0x0D53 Luxottica, 0x058E Meta Platforms Technologies, 0x01AB Meta
// Platforms; 16-bit service UUIDs 0xFD5F (Meta Platforms Technologies),
// 0xFEB7 / 0xFEB8 (Meta Platforms). IEEE: Luxottica 98:59:49 and Luxottica
// Tristar (the glasses plant) 80:AA:1C, 38:47:12. The composite is the
// precise match; 01AB / FEB7 / FEB8 also fire on other Meta hardware.
const PresetEntry PRESET_META[] = {
    { FT_META_COMPOSITE,  "0x0D53+0xFD5F", "Luxottica CID + Meta svc, or glasses name" },
    { FT_COMPANY_ID,      "0D53", "Luxottica CID (Ray-Ban / Oakley Meta glasses)" },
    { FT_SERVICE_UUID_16, "FD5F", "Meta Reality Labs svc UUID (glasses, Quest)" },
    { FT_COMPANY_ID,      "058E", "Meta Platforms Technologies CID (glasses, Quest)" },
    { FT_COMPANY_ID,      "01AB", "Meta Platforms CID (any Meta device)" },
    { FT_SERVICE_UUID_16, "FEB7", "Meta Platforms svc UUID (any Meta device)" },
    { FT_SERVICE_UUID_16, "FEB8", "Meta Platforms svc UUID (any Meta device)" },
    { FT_NAME_SUBSTRING,  "Ray-Ban", "Ray-Ban Meta glasses (BLE name)" },
    { FT_NAME_SUBSTRING,  "Oakley", "Oakley Meta glasses (BLE name)" },
    { FT_NAME_SUBSTRING,  "Wayfarer", "Ray-Ban Meta Wayfarer (BLE name)" },
    { FT_MAC_PREFIX,      "985949", "Luxottica Group OUI (IEEE)" },
    { FT_MAC_PREFIX,      "80AA1C", "Luxottica Tristar OUI (IEEE, glasses plant)" },
    { FT_MAC_PREFIX,      "384712", "Luxottica Tristar OUI (IEEE, glasses plant)" },
};
const size_t PRESET_META_COUNT = sizeof(PRESET_META) / sizeof(PRESET_META[0]);

// Axon body cameras (Body 3/4, Fleet dash, Taser 7/10).
// Uses all five signal types: dedicated IEEE OUI 00:25:DF ("Axon
// Enterprise, Inc."), Bluetooth SIG company ID 0x034D ("TASER
// International, Inc." — Axon's earlier registered name), service
// UUID 0xFC81 ("Axon Enterprise, Inc."), WiFi probe requests, and
// WiFi beacons. All uniquely attributable.
const PresetEntry PRESET_AXON[] = {
    { FT_MAC_PREFIX,      "0025DF", "Axon Enterprise OUI (IEEE)" },
    { FT_COMPANY_ID,      "034D",   "TASER International CID (Axon body cams)" },
    { FT_SERVICE_UUID_16, "FC81",   "Axon Enterprise service UUID" },
    { FT_WIFI_PROBE,      "0025DF", "Axon body cam (WiFi probe)" },
    { FT_WIFI_BEACON,     "0025DF", "Axon AP beacon (WiFi)" },
};
const size_t PRESET_AXON_COUNT = sizeof(PRESET_AXON) / sizeof(PRESET_AXON[0]);

// I-PRO body cameras — Bluetooth LE + WiFi domain. Panasonic subsidiary with 
// dedicated OUI.
// Preset is for both BLE advertisement MACs and WiFi probe requests.
const PresetEntry PRESET_IPRO[] = {
    { FT_MAC_PREFIX,   "D42DC5", "I-PRO body cam (BLE OUI)" },
    { FT_WIFI_PROBE,   "D42DC5", "I-PRO body cam (WiFi probe)" },
};
const size_t PRESET_IPRO_COUNT = sizeof(PRESET_IPRO) / sizeof(PRESET_IPRO[0]);

// Verkada cloud security cameras (CD/CM/CF series). Dedicated IEEE OUI
// E0:A7:00 registered to "Verkada Inc" (MA-L, registered 2016-09-22).
// Cameras are WiFi/Ethernet stations — no BLE advertising — so the preset
// is WiFi domain only: probe requests while disassociated, beacons from
// the setup/provisioning AP.
const PresetEntry PRESET_VERKADA[] = {
    { FT_WIFI_PROBE,  "E0A700", "Verkada camera (WiFi probe)" },
    { FT_WIFI_BEACON, "E0A700", "Verkada camera (WiFi beacon / setup AP)" },
};
const size_t PRESET_VERKADA_COUNT = sizeof(PRESET_VERKADA) / sizeof(PRESET_VERKADA[0]);

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

// Promiscuous callback — runs in driver context. No String, no Serial, no
// delay, no heap, and no devices[] access (that vector is owned by loop()).
// Fast path: frame-control check + 3-byte OUI pre-check against
// wifiOuiTable. Slow path: full filter match + enqueue — dedup and cooldown
// happen when loop() drains the entry (registerDetection), same as BLE.
static void handleWifiMatch(const uint8_t mac[6], uint8_t subtype, int8_t rssi,
                            const uint8_t* frame, int frameLen, uint8_t rxChan) {
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

    DevInfo di;
    devinfoFromWifi(di, frame, frameLen, subtype, rxChan);
    enqueueDetection(macStr, ident, desc, rssi, outType, true, &di);
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

    // Mesh RSSI sniff: ESP-NOW frames are 802.11 action frames, category
    // 0x7F (vendor specific) with Espressif's OUI 18:FE:34 right after the
    // MAC header. Recorded by transmitter address (addr2): with a fleet key
    // the payload is sealed and has no visible marker. Only fires during
    // sweeps, where promiscuous mode is active. No heap — driver context.
    if (ftype == 0 && subtype == 0x0D && pkt->rx_ctrl.sig_len > 28) {
        const uint8_t* p = pkt->payload;
        if (p[24] == 0x7F && p[25] == 0x18 && p[26] == 0xFE && p[27] == 0x34) {
            meshSniffRssi(hdr->addr2, pkt->rx_ctrl.rssi);
        }
    }

    // Drone Remote ID rides in beacons (vendor IE) and NAN action frames, from
    // any vendor: checked before the target-OUI prefilter.
    if (ridEnabled && ftype == 0 && (subtype == 0x08 || subtype == 0x0D)) {
        ridWifiFrame(pkt->payload, (int)pkt->rx_ctrl.sig_len - 4, rssi);
    }

    if (ftype == 0 && wdOn()) wdWifiFrame(pkt->payload, (int)pkt->rx_ctrl.sig_len - 4, subtype, rssi, pkt->rx_ctrl.channel);
    // 802.11 mgmt subtypes: 0x00=Assoc Req, 0x02=Reassoc Req, 0x04=Probe Req,
    // 0x05=Probe Resp, 0x08=Beacon. (Re)assoc requests are client frames like
    // probes (they name the AP the device joins); probe responses are AP frames
    // like beacons (they carry the SSID even when the beacon hides it).
    if (ftype != 0) return;  // management only
    if (subtype != 0x04 && subtype != 0x08 && subtype != 0x00 &&
        subtype != 0x02 && subtype != 0x05) return;

    // Source MAC is always in addr2 for probe requests and beacons.
    // 3-byte OUI pre-check against target table.
    uint8_t oui[3] = { hdr->addr2[0], hdr->addr2[1], hdr->addr2[2] };
    FilterReadGuard guard;
    if (!guard.ok) return;   // a live filter edit is swapping the list
    if (!wifiOuiInTargets(oui)) return;

    // Rare path: full match + enqueue (fixed stack buffers only)
    int flen = (int)pkt->rx_ctrl.sig_len - 4;   // sig_len counts the FCS
    handleWifiMatch(hdr->addr2, subtype, rssi, pkt->payload, flen, pkt->rx_ctrl.channel);
}

// Everything a BLE advert (plus scan response, active scan) says about the device.
static void devinfoFromBle(DevInfo& di, NimBLEAdvertisedDevice* dev) {
    if (dev->haveName()) {
        std::string nm = dev->getName();
        if (nm.length()) di.add(DI_NAME, nm.data(), nm.length() > 29 ? 29 : nm.length());
    }
    uint8_t fl = (dev->getAddressType() != BLE_ADDR_PUBLIC ? 1 : 0) | (dev->isConnectable() ? 2 : 0);
    di.add8(DI_BLEFLAGS, fl);
    di.add8(DI_ADVTYPE, dev->getAdvType());
    if (dev->haveTXPower()) di.add8(DI_TXPWR, (uint8_t)dev->getTXPower());
    if (dev->haveAppearance()) di.add16(DI_APPEAR, dev->getAppearance());
    if (dev->haveManufacturerData()) {
        std::string m = dev->getManufacturerData();
        if (m.length() >= 2) di.add(DI_MFR, m.data(), m.length() > 22 ? 22 : m.length());
    }
    uint8_t u16[16]; uint8_t nu = 0;
    bool full128 = false;
    for (int i = 0; i < (int)dev->getServiceUUIDCount() && nu < 16; i++) {
        NimBLEUUID u = dev->getServiceUUID(i);
        uint8_t bl = u.bitSize();
        if (bl == 16) {
            uint16_t x = u.getNative()->u16.value;
            u16[nu++] = x & 0xFF; u16[nu++] = x >> 8;
        } else if (bl == 128 && !full128) {
            // NimBLE stores 128-bit UUIDs little-endian; keep string order.
            const uint8_t* v = u.getNative()->u128.value;
            uint8_t be[16];
            for (int k = 0; k < 16; k++) be[k] = v[15 - k];
            if (!memcmp(be + 4, "\x00\x00\x10\x00\x80\x00\x00\x80\x5f\x9b\x34\xfb", 12)) {
                u16[nu++] = be[3]; u16[nu++] = be[2];   // Bluetooth base UUID: it is really 16-bit
            } else {
                di.add(DI_UUID128, be, 16);
                full128 = true;
            }
        } else if (bl == 32) {
            uint32_t x = u.getNative()->u32.value;
            u16[nu++] = x & 0xFF; u16[nu++] = (x >> 8) & 0xFF;
        }
    }
    if (nu) di.add(DI_UUID16, u16, nu);
    for (int i = 0; i < (int)dev->getServiceDataCount() && i < 2; i++) {
        NimBLEUUID u = dev->getServiceDataUUID(i);
        if (u.bitSize() != 16) continue;
        std::string d = dev->getServiceData(i);
        uint8_t t[14];
        uint16_t x = u.getNative()->u16.value;
        t[0] = x & 0xFF; t[1] = x >> 8;
        uint8_t dl = d.length() > 12 ? 12 : d.length();
        memcpy(t + 2, d.data(), dl);
        di.add(DI_SVCDATA, t, 2 + dl);
    }
}

// Forward declaration for callback class
class MyAdvertisedDeviceCallbacks;

// BLE Advertised Device Callback Class
class MyAdvertisedDeviceCallbacks: public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        if (currentMode != SCANNING_MODE) return;

        String mac = advertisedDevice->getAddress().toString().c_str();
        int rssi = advertisedDevice->getRSSI();

        {
            // Drone Remote ID (BLE legacy adverts, service data 0xFFFA).
            // Keep the address object alive: getAddress() returns a temporary, and a pointer
            // into it dangled (the bytes then came back mirrored: D4:2A:E7:E7:2A:D4).
            NimBLEAddress addr = advertisedDevice->getAddress();
            const uint8_t* le = addr.getNative();
            uint8_t be[6];
            for (int k = 0; k < 6; k++) be[k] = le[5 - k];
            if (ridEnabled) ridBleAdvert(advertisedDevice->getPayload(), (int)advertisedDevice->getPayloadLength(), be, rssi);
            if (wdOn()) {
                std::string nm = advertisedDevice->haveName() ? advertisedDevice->getName() : std::string();
                std::string md = advertisedDevice->haveManufacturerData() ? advertisedDevice->getManufacturerData() : std::string();
                bool hasMfr = md.length() >= 2;
                uint16_t mfr = hasMfr ? (uint8_t)md[0] | ((uint8_t)md[1] << 8) : 0;
                wdBleAdvert(be, advertisedDevice->getAddressType() != BLE_ADDR_PUBLIC, rssi, nm.c_str(), hasMfr, mfr,
                            advertisedDevice->haveAppearance() ? advertisedDevice->getAppearance() : 0,
                            advertisedDevice->getPayload(), (int)advertisedDevice->getPayloadLength());
            }
        }

        String matchedDescription;
        String matchedIdent;
        FilterType matchedTypeOut = FT_MAC_PREFIX;
        bool matchFound = false;
        {
            FilterReadGuard guard;   // skipped while a live filter edit swaps the list
            if (guard.ok) matchFound = matchesTargetFilter(advertisedDevice, mac,
                                                           matchedDescription, matchedIdent,
                                                           matchedTypeOut);
        }

        // Meta / Ray-Ban composite detection runs inside matchesTargetFilter
        // when (and only when) the META preset is installed — no filter, no
        // trigger.

        if (matchFound) {
            // Match-and-enqueue only. Device bookkeeping, cooldown, alerts,
            // MQTT and the session file all happen in loop() when the entry
            // is drained (registerDetection) — this callback never touches
            // devices[], which the WiFi callback used to read concurrently.
            DevInfo di;
            devinfoFromBle(di, advertisedDevice);
            enqueueDetection(mac.c_str(), matchedIdent.c_str(), matchedDescription.c_str(), rssi,
                             matchedTypeOut, false, &di);
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
static const unsigned long MESH_CH_DWELL_MS = 600;  // extra listen + mesh flush time while sweeping past the mesh channel
static const unsigned long MESH_VISIT_MS    = 100;  // mesh-channel visit between foreign channels (mesh on)
// With the mesh on, the sweep goes back to the mesh channel between every
// foreign channel, so no node is ever off the mesh for more than one hop
// (~200 ms): detections and their retries land near-instantly instead of
// waiting out a whole off-channel stretch of a peer's sweep.
static bool g_meshVisit = false;

// Dual-domain time-slice architecture:
// BLE and WiFi share the ESP32-S3's single 2.4 GHz antenna; simultaneous promiscuous
// RX + BLE scan is rated "C1" (unstable) per ESP-IDF. We time-slice to avoid: WiFi sweep
// (hop ch 11→1 at 200ms) and BLE scan (single 2s active scan) alternate, each ~2.2s.
// STA association is mutually exclusive with promiscuous RX on this hardware, so STA
// must be disconnected first. Alerting (beeps, LED) runs in the main loop's ring-buffer
// drain, not in NimBLE/WiFi callbacks. The promiscuous callback runs in driver context and must reject frames
// cheaply, so a 3-byte OUI table (wifiOuiTable) is rebuilt when WiFi filters change.
// One-time scanning init (idempotent). Must run before any scan_to_* call.
// Sequence: stop config services -> compute filter presence -> init BLE -> set up WiFi -> idle.
static void scanDomains(bool& wifi, bool& ble) {
    wifi = ble = false;
    for (const TargetFilter& f : targetFilters) {
        if (isWifiDomain(f.type)) wifi = true;
        else ble = true;
    }
    if (ridEnabled || wdOn()) wifi = ble = true;
#ifdef SKYSPY_NODE
    // A Sky-Spy node does what Sky-Spy does: no channel sweep. It stays parked on the
    // mesh channel (put the fleet on channel 6, where Remote ID's WiFi NAN and most
    // beacons are) with WiFi capture on and a continuous BLE scan alongside.
    wifi = false;
    ble = true;
#endif
}

#ifdef SKYSPY_NODE
// The Sky-Spy node lives on channel 6 (Remote ID's WiFi NAN channel, where most beacon
// Remote ID is too) and steps onto the mesh channel only to send: right after a drone
// report is queued, when a command or reply needs the air, and for a short window
// (announces, the dashboard's commands, OTA): 300 ms a second while idle, 200 ms every
// 2 s while a drone is up (~90% of its time on channel 6).
// With the whole mesh on channel 6 it never has to leave.
static void skyspyRadioTick() {
    const uint8_t RID_CH = 6;
    if (!meshCfg.enabled || currentMode != SCANNING_MODE || meshCfg.channel == RID_CH) return;
    static bool onMesh = true;
    static uint32_t windowUntil = 0, nextWindow = 0, parkedAt = 0;
    uint32_t now = millis();
    if ((int32_t)(now - nextWindow) >= 0) {
        // Idle (no drone in the last minute): 300 ms on the mesh every second, so
        // commands and update offers (repeated ~7 times over ~3.6 s) always land.
        // With a drone up: 200 ms every 2 s, ~90% of the time listening on 6.
        bool hunting = ridActive() > 0;
        windowUntil = now + (hunting ? 200 : 300);
        nextWindow = now + (hunting ? 2000 : 1000);
    }
    bool want = (int32_t)(windowUntil - now) > 0 || now - ridLastSendMs < 60 || meshNeedsAir() ||
                meshOtaBusy();
    if (onMesh && want && !meshOtaBusy() && now - parkedAt > 600 && (int32_t)(windowUntil - now) <= 0)
        want = false;   // never camp on the mesh channel: back to listening after 600 ms
    if (want && !onMesh) {
        meshParkChannel();
        onMesh = true;
        parkedAt = now;
    } else if (!want && onMesh) {
        esp_wifi_set_channel(RID_CH, WIFI_SECOND_CHAN_NONE);
        meshNoteRadioChannel(RID_CH);
        meshUnparkChannel();
        onMesh = false;
    }
}
#endif

// rid.cpp: a drone was heard for the first time. A Sky-Spy node alerts the way
// the standalone Sky-Spy firmware does; a detector stays quiet (the dashboard alerts).
void ridOnNewDrone() {
#ifdef SKYSPY_NODE
    startDetectionFlash();
    if (buzzerEnabled) threeBeeps();
#endif
}

static bool scan_init() {
    if (g_scanInitialized) return true;

    // (a) Stop config services
    dnsServer.stop();
    server.end();
    WiFi.softAPdisconnect(true);

    // (b) Compute filter presence
    bool hasWifiFilters, hasBleFilters;
    scanDomains(hasWifiFilters, hasBleFilters);

    // (c) BLE init — MUST happen before WiFi mode is set to STA/promiscuous
    if (hasBleFilters) {
        NimBLEDevice::init("");
        delay(1000);
        pBLEScan = NimBLEDevice::getScan();
        if (pBLEScan != nullptr) {
            pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
            pBLEScan->setActiveScan(true);
#ifdef SKYSPY_NODE
            pBLEScan->setDuplicateFilter(false);   // every advert: a drone's position each time it sends one
#endif
            pBLEScan->setInterval(300);
            pBLEScan->setWindow(200);
        }
    }

    // (d) WiFi setup. STA is kept up permanently only when MQTT needs it and
    // no WiFi filters are installed — BLE scans coexist with an associated
    // STA. With WiFi filters the sweeps own the radio: STA only comes up
    // during BLE phases, in bounded report windows (see mqttReportTick).
    // Mesh note: a remote node never runs the MQTT uplink (mesh is its
    // uplink), and a meshed home node can't keep STA up either — the radio
    // must park on the mesh channel between scans — so meshed nodes always
    // use report windows when they need STA.
    bool wantsMqtt = mqttCfg.enabled && mqttCfg.sta_ssid[0] && mqttCfg.broker[0]
                     && !meshIsRemoteUplink();
    g_mqttRadioTimeslice = wantsMqtt && (hasWifiFilters || meshCfg.enabled);
    if (hasWifiFilters || wantsMqtt || meshCfg.enabled) {
        WiFi.mode(WIFI_STA);
        if (wantsMqtt && !hasWifiFilters && !meshCfg.enabled) {
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

    // (d2) Node mesh: ESP-NOW LR on the STA interface, parked on the mesh
    // channel. Requires WiFi started; mesh_init() is a no-op when disabled.
    // Failure is non-fatal — scanning continues without the mesh.
    if (meshCfg.enabled) mesh_init();

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

    // The sweep hops channels, so the radio is no longer parked on the mesh
    // channel — mesh RX pauses until the next BLE phase (TX still lands on
    // the mesh channel; the broadcast peer is pinned there).
    meshUnparkChannel();
    meshSetBlePhase(false);

    g_radioPhase = PHASE_WIFI_SWEEP;
    g_wifiChannel = wdOn() ? wdFirstChannel() : 11;
    esp_wifi_set_channel(g_wifiChannel, WIFI_SECOND_CHAN_NONE);
    meshNoteRadioChannel(g_wifiChannel);
    // A stale "on a mesh visit" flag from the previous sweep made the urgent
    // hold below sit on channel 11 believing it was on the mesh.
    g_meshVisit = false;
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

    // Park the radio on the mesh channel for the whole BLE phase: the mesh
    // window. ESP-NOW RX works here alongside the BLE scan (coexistence
    // time-slices the two), and TX flush happens in meshTick().
    meshParkChannel();
    meshSetBlePhase(true);
#ifdef SKYSPY_NODE
    // ...and WiFi capture stays on: Remote ID beacons and NAN frames on this channel
    // are decoded while BLE scans (Sky-Spy runs both at once the same way).
    {
        wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
        esp_wifi_set_promiscuous_filter(&filt);
        esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
        esp_wifi_set_promiscuous(true);
    }
#endif

    g_radioPhase = PHASE_BLE_SCAN;
}

// Hop to the next WiFi channel while sweeping (11 -> 10 -> ... -> 1 -> 11).
static void scan_wifi_hop_channel() {
    if (g_radioPhase != PHASE_WIFI_SWEEP) return;
    if (wdOn()) g_wifiChannel = wdNextChannel(g_wifiChannel);   // the wardriver's plan (wd ch)
    else g_wifiChannel = (g_wifiChannel > 1) ? g_wifiChannel - 1 : 11;
    esp_wifi_set_channel(g_wifiChannel, WIFI_SECOND_CHAN_NONE);
    meshNoteRadioChannel(g_wifiChannel);
    g_meshVisit = false;
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

    mesh_deinit();
    WiFi.mode(WIFI_OFF);
    g_scanInitialized = false;
    g_radioPhase = PHASE_IDLE;
    if (isSerialConnected()) Serial.println("[SWEEP] Scanning exited");
}

// ================================
// Time-Sliced MQTT Reporting
// ================================
//
// With WiFi filters installed, promiscuous sweeps own the radio and the STA
// can only exist while promiscuous is off — i.e. during BLE phases.
// Detections that land during sweeps are queued as JSON payloads and flushed
// by this state machine during a BLE phase, after the 2 s scan window has
// expired. Every step is wall-clock bounded (STA association timeout below;
// mqtt_connect() adds its own TCP/CONNACK timeouts), so a missing broker or
// hung DHCP can delay the next sweep but can never wedge scanning.
//
// WiFi-only configs never reach a BLE phase, so their events stay queued and
// are eventually dropped past the cap — loop() logs this once, and the
// README documents it. BLE-only configs don't use this path at all: STA
// stays up and the drain publishes directly.

enum MqttReportState { MR_IDLE, MR_CONNECTING, MR_PUBLISHING };
static MqttReportState g_mqttReportState = MR_IDLE;
static unsigned long g_mqttReportStart = 0;
static const unsigned long MQTT_STA_TIMEOUT_MS = 2500;

// Queued detection payloads awaiting a report window. loop()-context only.
static const int MQTT_PENDING_SIZE = 8;
static char mqttPending[MQTT_PENDING_SIZE][256];
static int mqttPendingCount = 0;

static bool mqttPendingPush(const String& payload) {
    if (mqttPendingCount >= MQTT_PENDING_SIZE) return false;
    strncpy(mqttPending[mqttPendingCount], payload.c_str(), 255);
    mqttPending[mqttPendingCount][255] = '\0';
    mqttPendingCount++;
    return true;
}

// Called every scanning-loop iteration. `canOpenWindow` is true only while
// promiscuous is off and the BLE scan window has expired. A running cycle
// freezes phase transitions (loop() gates on g_mqttReportState == MR_IDLE)
// so the radio stays off promiscuous until the flush finishes.
static void mqttReportTick(unsigned long now, bool canOpenWindow) {
    switch (g_mqttReportState) {
        case MR_IDLE:
            if (canOpenWindow && mqttPendingCount > 0) {
                if (isSerialConnected()) Serial.println("[MQTT] report window opening (STA up)");
                meshNoteRadioChannel(0);   // the AP picks the channel now: unknown
                WiFi.begin(mqttCfg.sta_ssid, mqttCfg.sta_pass);
                g_mqttReportStart = now;
                g_mqttReportState = MR_CONNECTING;
            }
            break;

        case MR_CONNECTING:
            if (WiFi.status() == WL_CONNECTED) {
                mqtt_connect();
                g_mqttReportState = MR_PUBLISHING;
            } else if (now - g_mqttReportStart > MQTT_STA_TIMEOUT_MS) {
                if (isSerialConnected()) Serial.println("[MQTT] report window: STA timeout, dropping queued events");
                WiFi.disconnect();
                meshParkChannel();   // restore the mesh window
                mqttPendingCount = 0;
                g_mqttReportState = MR_IDLE;
            }
            break;

        case MR_PUBLISHING: {
            int sent = 0;
            if (mqttConnected) {
                for (int i = 0; i < mqttPendingCount; i++) {
                    mqtt_publish(mqttCfg.topic, mqttPending[i]);
                    sent++;
                }
            } else if (isSerialConnected()) {
                Serial.println("[MQTT] report window: broker connect failed, dropping queued events");
            }
            mqttPendingCount = 0;
            mqtt_disconnect();   // orderly DISCONNECT packet, then socket stop
            WiFi.disconnect();
            meshParkChannel();   // STA stole the channel; give it back
            if (isSerialConnected()) Serial.printf("[MQTT] report window closed (%d published)\n", sent);
            g_mqttReportState = MR_IDLE;
            // Resume scanning immediately rather than letting the BLE phase
            // clock run out on its own. Nodes without WiFi filters go back
            // to the BLE phase (which is also the mesh window).
            bool hasWifiFilters, hasBleUnused;
            scanDomains(hasWifiFilters, hasBleUnused);
            if (hasWifiFilters) {
                scan_to_wifi_sweep();
            } else {
                scan_to_ble_scan();
            }
            g_phaseStartTime = millis();
            break;
        }
    }
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

    // Also drop the rolling session file so the dashboard is genuinely
    // empty and a reboot doesn't resurrect the just-cleared list.
    clearCurrentSessionFile();

    if (isSerialConnected()) {
        Serial.println("All detected devices cleared from memory, NVS, and LittleFS");
    }
}

// ================================
// LittleFS Session Persistence
// ================================
//
// Two-file rolling scheme:
//   /session_curr.json — this boot's rolling detections, rewritten on
//                        every NEW hit (not on re-hits within cooldown).
//   /session_prev.json — the previous boot's session, promoted from
//                        session_curr on startup and read-only until the
//                        next reboot rotates it out.
//
// Cap: SESSION_MAX_ENTRIES per file; LRU-drop happens in registerDetection()
// when devices[] grows past the cap so the JSON never gets huge.

static const char*  SESSION_CURR_PATH   = "/session_curr.json";
static const char*  SESSION_PREV_PATH   = "/session_prev.json";
static const size_t SESSION_MAX_ENTRIES = 200;
static const size_t SESSION_JSON_BYTES  = 48 * 1024;

static bool sessionFSReady = false;
volatile bool sessionDirty = false;   // set by registerDetection, drained in loop()

static void ensureSessionFS();
static bool filterFsReady() { ensureSessionFS(); return sessionFSReady; }
static void ensureSessionFS() {
    if (sessionFSReady) return;
    if (LittleFS.begin(true)) {       // format on failure
        sessionFSReady = true;
    } else if (isSerialConnected()) {
        Serial.println("[detector] LittleFS mount failed");
    }
}

// Boot-time rotation: promote last boot's live file to the prev slot,
// then wipe the live slot so this session starts clean. Logs a one-line
// summary so a serial capture can prove the persistence path is alive.
static void rotateSessionFiles() {
    ensureSessionFS();
    if (!sessionFSReady) return;

    if (LittleFS.exists(SESSION_CURR_PATH)) {
        if (LittleFS.exists(SESSION_PREV_PATH)) LittleFS.remove(SESSION_PREV_PATH);
        if (LittleFS.rename(SESSION_CURR_PATH, SESSION_PREV_PATH)) {
            if (isSerialConnected()) Serial.println("[detector] rotated session_curr -> session_prev");
        } else if (isSerialConnected()) {
            Serial.println("[detector] session rotate failed");
        }
    }

    if (LittleFS.exists(SESSION_PREV_PATH)) {
        File f = LittleFS.open(SESSION_PREV_PATH, "r");
        size_t entries = 0;
        if (f) {
            DynamicJsonDocument doc(SESSION_JSON_BYTES);
            DeserializationError err = deserializeJson(doc, f);
            f.close();
            if (!err && doc.is<JsonArray>()) {
                entries = doc.as<JsonArray>().size();
            } else if (isSerialConnected()) {
                Serial.println("[detector] session_prev parse failed, treating as empty");
            }
        }
        if (isSerialConnected()) {
            Serial.printf("[detector] session_prev loaded (%u entries)\n", (unsigned)entries);
        }
    } else if (isSerialConnected()) {
        Serial.println("[detector] no previous session");
    }
}

// Serialise devices[] out as a plain JSON array. Wrap-crashing writes
// aren't defended against — the load path tolerates a corrupt file and
// falls back to empty, which is fine for a detector.
static void writeCurrentSession() {
    ensureSessionFS();
    if (!sessionFSReady) return;

    DynamicJsonDocument doc(SESSION_JSON_BYTES);
    JsonArray arr = doc.to<JsonArray>();

    size_t start = devices.size() > SESSION_MAX_ENTRIES
                    ? devices.size() - SESSION_MAX_ENTRIES : 0;
    for (size_t i = start; i < devices.size(); i++) {
        const DeviceInfo& d = devices[i];
        JsonObject o = arr.createNestedObject();
        o["mac"]      = d.macAddress;
        o["first_ms"] = d.firstSeen;
        o["last_ms"]  = d.lastSeen;
        o["rssi"]     = d.rssi;
        o["type"]     = filterTypeCode(d.matchedType);
        o["match"]    = d.matchedIdentifier;
        o["desc"]     = d.filterDescription;
    }

    File f = LittleFS.open(SESSION_CURR_PATH, "w");
    if (!f) {
        if (isSerialConnected()) Serial.println("[detector] session_curr open failed");
        return;
    }
    serializeJson(doc, f);
    f.close();
}

static void clearCurrentSessionFile() {
    ensureSessionFS();
    if (sessionFSReady && LittleFS.exists(SESSION_CURR_PATH)) {
        LittleFS.remove(SESSION_CURR_PATH);
    }
}

static void clearPreviousSessionFile() {
    ensureSessionFS();
    if (sessionFSReady && LittleFS.exists(SESSION_PREV_PATH)) {
        LittleFS.remove(SESSION_PREV_PATH);
    }
}

// Slurp session_prev.json verbatim for the dashboard. Empty array on
// any error / missing file — the UI hides its panel when the array is
// empty, so this is the graceful fallback.
static String readPreviousSessionJson() {
    ensureSessionFS();
    if (!sessionFSReady || !LittleFS.exists(SESSION_PREV_PATH)) return "[]";
    File f = LittleFS.open(SESSION_PREV_PATH, "r");
    if (!f) return "[]";
    String out;
    out.reserve(f.size() + 2);
    while (f.available()) out += (char)f.read();
    f.close();
    if (out.length() == 0) out = "[]";
    return out;
}

// ================================
// Detection Registration (loop context only)
// ================================

// Single-threaded owner of devices[]. Called from loop() when draining the
// detection ring — never from a BLE or WiFi callback — so the vector and
// its Strings are only ever touched by the loop task (the WiFi promiscuous
// callback used to iterate this vector from the driver task while the BLE
// callback mutated it: a use-after-free waiting to happen). Returns the
// alert label ("NEW" / "RE-3s" / "RE-30s"), "UPD" (still in view: a quiet
// signal update every SIGNAL_UPDATE_MS, no alert) or nullptr when the hit is
// suppressed by the cooldown ladder. WiFi matches key on (MAC, filter type)
// so one camera can register separately as probe source and beacon source;
// BLE matches keep the historic MAC-only key.
#define SIGNAL_UPDATE_MS 5000UL
static const char* registerDetection(const char* mac, FilterType type, int rssi,
                                     bool wifiDomain, const String& ident,
                                     const String& desc) {
    unsigned long now = millis();

    for (auto& dev : devices) {
        if (strcmp(dev.macAddress.c_str(), mac) != 0) continue;
        if (wifiDomain && dev.matchedType != type) continue;

        if (dev.inCooldown && now < dev.cooldownUntil) {
            // Still in view during the cooldown: keep tracking it, so it does
            // not look like it just came back when the cooldown ends (that
            // re-alerted a device that never left, and ate its updates).
            dev.lastSeen = now;
            dev.rssi = rssi;
            if (now - dev.lastReport >= SIGNAL_UPDATE_MS) {
                dev.lastReport = now;
                return "UPD";
            }
            return nullptr;
        }
        if (dev.inCooldown) dev.inCooldown = false;

        unsigned long timeSinceLastSeen = now - dev.lastSeen;
        dev.lastSeen = now;
        dev.rssi = rssi;

        if (timeSinceLastSeen >= 30000) {
            dev.inCooldown = true;
            dev.cooldownUntil = now + 10000;
            dev.lastReport = now;
            return "RE-30s";
        }
        if (timeSinceLastSeen >= 3000) {
            dev.inCooldown = true;
            dev.cooldownUntil = now + 3000;
            dev.lastReport = now;
            return "RE-3s";
        }
        // Continuously in view: the ladder stays quiet (no beeps), but the
        // dashboard still gets the signal every few seconds so it can tell
        // which node is closest, and track it, in real time.
        if (now - dev.lastReport >= SIGNAL_UPDATE_MS) {
            dev.lastReport = now;
            return "UPD";
        }
        return nullptr;
    }

    DeviceInfo newDev;
    newDev.macAddress = mac;
    newDev.rssi = rssi;
    newDev.firstSeen = now;
    newDev.lastSeen = now;
    newDev.lastReport = now;
    newDev.inCooldown = false;
    newDev.cooldownUntil = 0;
    newDev.matchedFilter = desc.c_str();
    newDev.filterDescription = desc;
    newDev.matchedIdentifier = ident;
    newDev.matchedType = type;
    devices.push_back(newDev);

    // LRU-drop oldest so the session cap holds even with a
    // firehose of unique MACs.
    while (devices.size() > 200) {
        devices.erase(devices.begin());
    }

    sessionDirty = true;  // main loop will flush to LittleFS
    return "NEW";
}

// ================================
// Web Server HTML
// ================================
const char* getASCIIArt() {
    return R"(
 ████  █   █ █   █     ████  ████  █   █
█    █ █   █ █   █    █      █   █ █   █
█    █ █   █ █   █     ████  ████   █ █
█    █ █   █ █   █        █ █       █ █
█    █ █   █ █   █        █ █        █
 ████   ████  ████     ████  █        █
)";
}

const char CONFIG_HTML[] PROGMEM =
R"html(
<!DOCTYPE html>
<html>
<head>
    <title>OUI-SPY Detector</title>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        :root {
            --bg: #06080b;
            --panel: #0c0f14;
            --panel-2: #10141b;
            --line: rgba(148, 163, 184, 0.12);
            --line-strong: rgba(148, 163, 184, 0.24);
            --text: #dbe4ee;
            --soft: #9ecfff;
            --muted: #8593a6;
            --faint: #5b6879;
            --accent: #4ecdc4;
            --accent-dim: rgba(78, 205, 196, 0.10);
            --amber: #fbbf24;
            --red: #f87171;
            --mono: ui-monospace, "SF Mono", "Cascadia Code", Consolas, monospace;
            --sans: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
        }
        * { box-sizing: border-box; }
        body {
            font-family: var(--sans);
            margin: 0; padding: 18px 12px;
            overflow-x: hidden;
            background: var(--bg);
            color: var(--text);
            font-size: 14px; line-height: 1.5;
            background-image: radial-gradient(ellipse 700px 340px at 15%% -8%%, rgba(78,205,196,0.06), transparent);
            background-repeat: no-repeat;
        }
        .container { max-width: 660px; margin: 0 auto; background: transparent; padding: 0; border: none; box-shadow: none; backdrop-filter: none; }
        h1 {
            font-size: 22px; font-weight: 800; letter-spacing: 6px;
            color: #ffffff; text-transform: uppercase;
            margin: 2px 0 4px; padding-bottom: 12px;
            border-bottom: 1px solid var(--line);
        }
        @media (max-width: 768px) {
            h1 { font-size: 17px; letter-spacing: 3px; }
            body { padding: 16px 10px; }
            .container { padding: 0; margin: 0; }
        }
        .section {
            margin-bottom: 12px; padding: 14px 16px;
            border: 1px solid var(--line); border-radius: 10px;
            background: var(--panel);
        }
        .section h3 {
            margin: 0 0 10px; font-size: 11px; font-weight: 700;
            letter-spacing: 2.5px; text-transform: uppercase; color: var(--accent);
        }
        textarea {
            width: 100%%; min-height: 110px; padding: 12px;
            border: 1px solid var(--line-strong); border-radius: 8px;
            background: #090c11; color: var(--text);
            font-family: var(--mono); font-size: 13px; resize: vertical;
        }
        textarea:focus { outline: none; border-color: var(--accent); box-shadow: 0 0 0 3px var(--accent-dim); }
        .field, input[type="text"], input[type="password"], input[type="number"], select {
            width: 100%%; padding: 8px 10px; margin-bottom: 10px;
            border: 1px solid var(--line-strong); border-radius: 7px;
            background: #090c11; color: var(--text);
            font-size: 13px; font-family: var(--sans);
        }
        .field:focus, input[type="text"]:focus, input[type="password"]:focus, input[type="number"]:focus, select:focus {
            outline: none; border-color: var(--accent); box-shadow: 0 0 0 3px var(--accent-dim);
        }
        select option { background: #0c0f14; color: var(--text); }
        .help-text { font-size: 12px; color: var(--muted); margin-top: 8px; line-height: 1.5; }
        .toggle-container { display: flex; flex-direction: column; gap: 10px; }
        .toggle-item {
            display: flex; align-items: center; gap: 12px; padding: 9px 12px;
            border: 1px solid var(--line); border-radius: 8px; background: var(--panel-2);
        }
        input[type="checkbox"] {
            appearance: none; -webkit-appearance: none;
            width: 17px; height: 17px; flex: none; margin: 0;
            border: 1px solid var(--line-strong); border-radius: 4px;
            background: #161b23; cursor: pointer; position: relative;
        }
        input[type="checkbox"]:checked { background: var(--accent); border-color: var(--accent); }
        input[type="checkbox"]:checked::after {
            content: ""; position: absolute; left: 5px; top: 1.5px;
            width: 4px; height: 9px;
            border: solid #052723; border-width: 0 2px 2px 0;
            transform: rotate(45deg);
        }
        .toggle-label { font-weight: 500; color: var(--soft); cursor: pointer; user-select: none; font-size: 14px; }
        button {
            background: var(--accent); color: #052723;
            padding: 9px 18px; border: none; border-radius: 7px;
            cursor: pointer; font-size: 13px; font-weight: 700;
            font-family: var(--sans); margin: 6px 4px; transition: filter .15s, transform .15s, box-shadow .15s;
        }
        button:hover { filter: brightness(1.12); transform: translateY(-1px); box-shadow: 0 4px 18px rgba(78, 205, 196, 0.28); }
        .button-container { text-align: center; margin-top: 24px; padding-top: 20px; border-top: 1px solid var(--line); }
        .status {
            padding: 14px 16px; border-radius: 8px; margin: 10px 0 24px;
            background: var(--accent-dim); border: 1px solid rgba(78, 205, 196, 0.28);
            color: var(--text); font-size: 13px;
        }
        .oui-db details { margin-bottom: 6px; border: 1px solid var(--line); border-radius: 8px; background: var(--panel-2); overflow: hidden; }
        .oui-db summary { cursor: pointer; padding: 11px 12px; transition: background .15s; list-style: none; font-size: 13px; color: var(--muted); }
        .oui-db summary b { color: var(--soft); font-weight: 600; letter-spacing: 0.4px; }
        .oui-db summary code {
            background: rgba(148, 163, 184, 0.08); border: 1px solid var(--line);
            color: var(--faint); border-radius: 9px; padding: 1px 8px;
            font-size: 10px; margin-left: 8px; font-family: var(--mono);
        }
        .oui-db summary:active { background: rgba(148, 163, 184, 0.08); }
        .oui-db details[open] > summary { color: var(--muted); }
        .oui-db details[open] > summary b { color: var(--accent); }
        .oui-db summary::-webkit-details-marker { display: none; }
        .oui-db summary::before { content: "\25B8"; color: var(--accent); margin-right: 10px; font-size: 10px; }
        .oui-db details[open] summary::before { content: "\25BE"; }
        .oui-db summary:hover { background: rgba(148, 163, 184, 0.05); }
        .oui-db details[open] { border-color: var(--line-strong); }
        .oui-db details { padding-bottom: 6px; }
        .oui-db .oui-entries { padding: 2px 14px 8px; line-height: 2.1; }
        .oui-db .oui-entries code {
            display: inline-block; margin: 2px 4px 2px 0; padding: 2px 8px;
            background: rgba(78, 205, 196, 0.07); border: 1px solid rgba(78, 205, 196, 0.16);
            border-radius: 4px; font-size: 12px; color: var(--accent); font-family: var(--mono);
        }
        .sig-lines { margin-top: 12px; }
        .sig-line {
            display: flex; align-items: center; flex-wrap: wrap; gap: 6px;
            padding: 6px 8px; margin-bottom: 5px;
            background: var(--panel-2); border: 1px solid var(--line);
            border-radius: 8px; font-size: 12px; font-family: var(--mono);
        }
        .sig-vendor { font-weight: 700; color: #ffffff; margin-right: 4px; font-family: var(--sans); font-size: 11px; letter-spacing: 1px; }
        .sig-mac  { color: #7dd3fc; }
        .sig-cid  { color: var(--amber); }
        .sig-uuid { color: #5eead4; }
        .sig-name { color: #c4b5fd; }
        .sig-meta { color: #f0abfc; }
        .sig-probe  { color: var(--accent); }
        .sig-beacon { color: var(--accent); }
        .sig-sep  { color: var(--faint); }
        .sig-rm { margin-left: auto; background: none; border: none; color: var(--muted); cursor: pointer; font-size: 14px; padding: 0 4px; line-height: 1; }
        .sig-rm:hover { background: none; box-shadow: none; filter: none; transform: none; color: var(--text); }
        .oui-add-btn {
            background: transparent !important; color: var(--accent) !important;
            border: 1px solid rgba(78, 205, 196, 0.35) !important;
            font-size: 12px !important; font-weight: 600 !important; letter-spacing: 0.5px;
            padding: 7px 14px !important; margin: 4px 14px 10px !important;
            width: auto; border-radius: 6px !important;
        }
        .oui-add-btn:hover { background: var(--accent-dim) !important; box-shadow: none !important; filter: none !important; transform: none !important; }
        .oui-db .oui-meta { font-size: 12px; color: var(--muted); margin: 2px 0; padding: 0 14px; }
        .oui-db .oui-note { font-size: 11px; color: var(--faint); margin: 4px 0; padding: 0 14px; font-style: italic; }
        .ascii-art {
            display: block;
            width: 100%;
            height: auto;
            margin: 4px auto 18px;
            /* the art is glowing lines on pure black; screen-blend drops
               the black out so it sits on our near-black page seamlessly */
            mix-blend-mode: screen;
        }
    </style>
</head>
<body>
    <div class="container">
        <img class="ascii-art" alt="OUI-SPY" src="data:image/png;base64,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">
        
                    <form id="lockForm" method="POST" action="/save" style="display:none">
                        <input type="hidden" name="burnin" value="1">
                        <input type="hidden" name="filters">
                        <input type="hidden" name="buzzerEnabled">
                        <input type="hidden" name="ledEnabled">
                        <input type="hidden" name="ap_ssid">
                        <input type="hidden" name="ap_password">
                        <input type="hidden" name="mqtt_en">
                        <input type="hidden" name="mq_id"><input type="hidden" name="mq_ss">
                        <input type="hidden" name="mq_sp"><input type="hidden" name="mq_bk">
                        <input type="hidden" name="mq_pt"><input type="hidden" name="mq_us">
                        <input type="hidden" name="mq_pw"><input type="hidden" name="mq_tp">
                        <input type="hidden" name="mesh_en">
                        <input type="hidden" name="mesh_role">
                        <input type="hidden" name="mesh_ch">
                        <input type="hidden" name="mesh_key">
                        <input type="hidden" name="mesh_key_clear">
                    </form>

        <form id="configForm" method="POST" action="/save" autocomplete="off">
            <input type="hidden" id="filtersJson" name="filters" value="">
            <div class="section">
                <h3>Detection Filters</h3>
                <div class="help-text" style="margin-bottom:12px;">
                    Match BLE advertisements and WiFi frames by OUI or full MAC — add manually below, or install a vendor from the OUI Database.
                </div>
                <div class="add-bar">
                    <input type="text" id="filterInput" class="field" placeholder="AA:BB:CC  or  AA:BB:CC:DD:EE:FF" maxlength="17" autocomplete="off"
                           onkeydown="if(event.key==='Enter'){event.preventDefault();addFilterFromInput();}">
                    <span class="filter-domains" id="addDomains" data-ble="true" data-wifi="true">
                        <span class="match-badge type-BLE" data-dom="ble" onclick="toggleAddDomain(this)">BLE</span>
                        <span class="match-badge type-WiFi" data-dom="wifi" onclick="toggleAddDomain(this)">WiFi</span>
                    </span>
                    <button type="button" class="add-btn" onclick="addFilterFromInput()">Add</button>
                </div>
                <div id="filterList">%FILTER_LIST_ROWS%</div>
                <div id="emptyFilterMsg" class="empty-state-message">No filters yet — add one above or pick a vendor below.</div>
                <div id="sigLines" class="sig-lines"></div>
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
                    <div class="oui-entries"><code data-oui="18:7F:88" onclick="toggleOuiChip(this)">18:7F:88</code> <code data-oui="24:2B:D6" onclick="toggleOuiChip(this)">24:2B:D6</code> <code data-oui="34:3E:A4" onclick="toggleOuiChip(this)">34:3E:A4</code> <code data-oui="54:E0:19" onclick="toggleOuiChip(this)">54:E0:19</code> <code data-oui="5C:47:5E" onclick="toggleOuiChip(this)">5C:47:5E</code> <code data-oui="64:9A:63" onclick="toggleOuiChip(this)">64:9A:63</code> <code data-oui="90:48:6C" onclick="toggleOuiChip(this)">90:48:6C</code> <code data-oui="9C:76:13" onclick="toggleOuiChip(this)">9C:76:13</code> <code data-oui="AC:9F:C3" onclick="toggleOuiChip(this)">AC:9F:C3</code> <code data-oui="C4:DB:AD" onclick="toggleOuiChip(this)">C4:DB:AD</code> <code data-oui="CC:3B:FB" onclick="toggleOuiChip(this)">CC:3B:FB</code></div>
                    <button type="button" class="oui-add-btn" data-ble="true" data-wifi="true" data-label="RING" onclick="addFilterCard(this)">+ Add selected to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Doorbell/Security Camera</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> Typical WiFi/BLE range</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Ring Doorbell, Ring Camera, Ring Chime</div>
                    </details>
                    <details>
                    <summary><b>FLOCK SAFETY</b> <code>1 OUI</code></summary>
                    <div class="oui-entries"><code data-oui="B4:1E:52" onclick="toggleOuiChip(this)">B4:1E:52</code></div>
                    <button type="button" class="oui-add-btn" data-ble="true" data-wifi="true" data-label="FLOCK SAFETY" onclick="addFilterCard(this)">+ Add selected to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Automated License Plate Reader (ALPR) / Security Camera</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi/Cellular</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Flock Safety Camera, Falcon Camera, Raven Camera</div>
                    </details>
                    <details>
                    <summary><b>FLOCK SAFETY (WiFi Promiscuous — @NitekryDPaul research)</b> <code>30 OUIs</code></summary>
                    <div class="oui-entries"><code data-oui="70:C9:4E" onclick="toggleOuiChip(this)">70:C9:4E</code> <code data-oui="3C:91:80" onclick="toggleOuiChip(this)">3C:91:80</code> <code data-oui="D8:F3:BC" onclick="toggleOuiChip(this)">D8:F3:BC</code> <code data-oui="80:30:49" onclick="toggleOuiChip(this)">80:30:49</code> <code data-oui="B8:35:32" onclick="toggleOuiChip(this)">B8:35:32</code> <code data-oui="14:5A:FC" onclick="toggleOuiChip(this)">14:5A:FC</code> <code data-oui="74:4C:A1" onclick="toggleOuiChip(this)">74:4C:A1</code> <code data-oui="08:3A:88" onclick="toggleOuiChip(this)">08:3A:88</code> <code data-oui="9C:2F:9D" onclick="toggleOuiChip(this)">9C:2F:9D</code> <code data-oui="C0:35:32" onclick="toggleOuiChip(this)">C0:35:32</code> <code data-oui="94:08:53" onclick="toggleOuiChip(this)">94:08:53</code> <code data-oui="E4:AA:EA" onclick="toggleOuiChip(this)">E4:AA:EA</code> <code data-oui="F4:6A:DD" onclick="toggleOuiChip(this)">F4:6A:DD</code> <code data-oui="F8:A2:D6" onclick="toggleOuiChip(this)">F8:A2:D6</code> <code data-oui="24:B2:B9" onclick="toggleOuiChip(this)">24:B2:B9</code> <code data-oui="00:F4:8D" onclick="toggleOuiChip(this)">00:F4:8D</code> <code data-oui="D0:39:57" onclick="toggleOuiChip(this)">D0:39:57</code> <code data-oui="E8:D0:FC" onclick="toggleOuiChip(this)">E8:D0:FC</code> <code data-oui="E0:4F:43" onclick="toggleOuiChip(this)">E0:4F:43</code> <code data-oui="B8:1E:A4" onclick="toggleOuiChip(this)">B8:1E:A4</code> <code data-oui="70:08:94" onclick="toggleOuiChip(this)">70:08:94</code> <code data-oui="58:8E:81" onclick="toggleOuiChip(this)">58:8E:81</code> <code data-oui="EC:1B:BD" onclick="toggleOuiChip(this)">EC:1B:BD</code> <code data-oui="3C:71:BF" onclick="toggleOuiChip(this)">3C:71:BF</code> <code data-oui="58:00:E3" onclick="toggleOuiChip(this)">58:00:E3</code> <code data-oui="90:35:EA" onclick="toggleOuiChip(this)">90:35:EA</code> <code data-oui="5C:93:A2" onclick="toggleOuiChip(this)">5C:93:A2</code> <code data-oui="64:6E:69" onclick="toggleOuiChip(this)">64:6E:69</code> <code data-oui="48:27:EA" onclick="toggleOuiChip(this)">48:27:EA</code> <code data-oui="A4:CF:12" onclick="toggleOuiChip(this)">A4:CF:12</code></div>
                    <button type="button" class="oui-add-btn" data-ble="true" data-wifi="true" data-label="FLOCK SAFETY (WiFi Promiscuous — @NitekryDPaul research)" onclick="addFilterCard(this)">+ Add selected to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Automated License Plate Reader (ALPR) / Security Camera</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi 2.4 GHz (promiscuous mode, addr1 + addr2)</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Flock Safety infrastructure (cameras, uplinks, peripherals)</div>
                    <div class="oui-meta"><strong>Research Credit:</strong> ØяĐöØцяöЪöяцฐ / <strong>@NitekryDPaul</strong> — identified these prefixes through 2.4 GHz promiscuous-mode analysis, including the addr1-receiver detection technique that catches Flock stations during their burst-sleep duty cycle</div>
                    </details>
                    <details>
                    <summary><b>AXON (BLE)</b> <code>1 OUI</code></summary>
                    <div class="oui-entries"><code data-oui="00:25:DF" onclick="toggleOuiChip(this)">00:25:DF</code></div>
                    <button type="button" class="oui-add-btn" data-ble="true" data-wifi="true" data-label="AXON (BLE)" onclick="addFilterCard(this)">+ Add selected to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Body Camera / Law Enforcement</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> BLE range — field-validated, stable non-randomised MAC in advertisements</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Axon Body 3/4, Fleet dash cam, Taser 7/10 — same IEEE block as the WiFi side; BT SIG company ID `0x034D` and service UUID `0xFC81` complete the signature (AXON preset installs all five)</div>
                    </details>
                    <details>
                    <summary><b>DJI</b> <code>8 OUIs</code></summary>
                    <div class="oui-entries"><code data-oui="0C:9A:E6" onclick="toggleOuiChip(this)">0C:9A:E6</code> <code data-oui="8C:58:23" onclick="toggleOuiChip(this)">8C:58:23</code> <code data-oui="04:A8:5A" onclick="toggleOuiChip(this)">04:A8:5A</code> <code data-oui="58:B8:58" onclick="toggleOuiChip(this)">58:B8:58</code> <code data-oui="E4:7A:2C" onclick="toggleOuiChip(this)">E4:7A:2C</code> <code data-oui="60:60:1F" onclick="toggleOuiChip(this)">60:60:1F</code> <code data-oui="48:1C:B9" onclick="toggleOuiChip(this)">48:1C:B9</code> <code data-oui="34:D2:62" onclick="toggleOuiChip(this)">34:D2:62</code></div>
                    <button type="button" class="oui-add-btn" data-ble="true" data-wifi="true" data-label="DJI" onclick="addFilterCard(this)">+ Add selected to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Consumer & Commercial Drones</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi/OcuSync up to several km</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Mavic, Phantom, Inspire, Mini series</div>
                    </details>
                    <details>
                    <summary><b>PARROT</b> <code>5 OUIs</code></summary>
                    <div class="oui-entries"><code data-oui="00:12:1C" onclick="toggleOuiChip(this)">00:12:1C</code> <code data-oui="00:26:7E" onclick="toggleOuiChip(this)">00:26:7E</code> <code data-oui="90:03:B7" onclick="toggleOuiChip(this)">90:03:B7</code> <code data-oui="90:3A:E6" onclick="toggleOuiChip(this)">90:3A:E6</code> <code data-oui="A0:14:3D" onclick="toggleOuiChip(this)">A0:14:3D</code></div>
                    <button type="button" class="oui-add-btn" data-ble="true" data-wifi="true" data-label="PARROT" onclick="addFilterCard(this)">+ Add selected to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Consumer & Commercial Drones</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi/BLE range</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Parrot Anafi, Parrot Bebop, Parrot AR.Drone</div>
                    </details>
                    <details>
                    <summary><b>SKYDIO</b> <code>1 OUI</code></summary>
                    <div class="oui-entries"><code data-oui="38:1D:14" onclick="toggleOuiChip(this)">38:1D:14</code></div>
                    <button type="button" class="oui-add-btn" data-ble="true" data-wifi="true" data-label="SKYDIO" onclick="addFilterCard(this)">+ Add selected to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Commercial & Enterprise Drones</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi range</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Skydio 2, Skydio X2, Skydio 3</div>
                    </details>
                    <details>
                    <summary><b>META/RAYBAN SMARTGLASSES</b> <code>7 OUIs</code></summary>
                    <div class="oui-note"><em>sourced from <a href=\"https://github.com/sh4d0wm45k/glass-detect/blob/main/glass-detect/glass-detect.ino#L21\" target=\"_blank\" style=\"color:#4ecdc4;\">glass-detect repository</a></em></div>
                    <div class="oui-note">Last one from Luxottica Group S.P.A. detected by @konradit.</div>
                    <div class="oui-entries"><code data-oui="7C:2A:9E" onclick="toggleOuiChip(this)">7C:2A:9E</code> <code data-oui="CC:66:0A" onclick="toggleOuiChip(this)">CC:66:0A</code> <code data-oui="F4:03:43" onclick="toggleOuiChip(this)">F4:03:43</code> <code data-oui="5C:E9:1E" onclick="toggleOuiChip(this)">5C:E9:1E</code> <code data-oui="98:59:49" onclick="toggleOuiChip(this)">98:59:49</code> <code data-oui="80:AA:1C" onclick="toggleOuiChip(this)">80:AA:1C</code> <code data-oui="38:47:12" onclick="toggleOuiChip(this)">38:47:12</code></div>
                    <button type="button" class="oui-add-btn" data-ble="true" data-wifi="true" data-label="META/RAYBAN SMARTGLASSES" onclick="addFilterCard(this)">+ Add selected to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Smartglasses</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> WiFi/BLE range</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Meta/Ray-Ban Smartglasses</div>
                    </details>
                    <details>
                    <summary><b>FLIPPER ZERO</b> <code>2 OUIs</code></summary>
                    <div class="oui-entries"><code data-oui="80:E1:26" onclick="toggleOuiChip(this)">80:E1:26</code> <code data-oui="80:E1:27" onclick="toggleOuiChip(this)">80:E1:27</code></div>
                    <button type="button" class="oui-add-btn" data-ble="true" data-wifi="true" data-label="FLIPPER ZERO" onclick="addFilterCard(this)">+ Add selected to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Pentest / Hacking Tool</div>
                    <div class="oui-meta"><strong>Detection Range:</strong> BLE range</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Flipper Zero (BLE address prefixes, community-reported; its BLE name also contains "Flipper")</div>
                    </details>
                    <!-- OUI_DB_END -->
                    <!-- WIFI_OUI_DB_START -->
                    <details>
                    <summary><b>AXON (WiFi)</b> <code>1 OUI</code></summary>
                    <div class="oui-entries"><code>00:25:DF</code></div>
                    <button type="button" class="oui-add-btn" onclick="addVendor('axon','AXON (WiFi)','00:25:DF')">+ Add all signatures</button>
                    <div class="oui-meta"><strong>Category:</strong> Body Camera / Law Enforcement</div>
                    <div class="oui-meta"><strong>Frame Types:</strong> Probe Request, Beacon</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Axon Body Camera (probe), Axon AP (beacon)</div>
                    </details>
                    <details>
                    <summary><b>I-PRO</b> <code>1 OUI</code></summary>
                    <div class="oui-entries"><code>D4:2D:C5</code></div>
                    <button type="button" class="oui-add-btn" onclick="addVendor('ipro','I-PRO','D4:2D:C5')">+ Add all signatures</button>
                    <div class="oui-meta"><strong>Category:</strong> Body Camera / Law Enforcement</div>
                    <div class="oui-meta"><strong>Frame Types:</strong> Probe Request</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> I-PRO Body Camera</div>
                    </details>
                    <details>
                    <summary><b>Verkada</b> <code>1 OUI</code></summary>
                    <div class="oui-entries"><code>E0:A7:00</code></div>
                    <button type="button" class="oui-add-btn" onclick="addVendor('verkada','Verkada','E0:A7:00')">+ Add all signatures</button>
                    <div class="oui-meta"><strong>Category:</strong> Security Cameras / Surveillance</div>
                    <div class="oui-meta"><strong>Frame Types:</strong> Probe Request, Beacon</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Verkada security cameras (CD/CM/CF series, WiFi models)</div>
                    </details>
                    <details>
                    <summary><b>WiFi PINEAPPLE (Hak5)</b> <code>1 OUI</code></summary>
                    <div class="oui-entries"><code data-oui="00:13:37" onclick="toggleOuiChip(this)">00:13:37</code></div>
                    <button type="button" class="oui-add-btn" data-ble="false" data-wifi="true" data-label="WiFi PINEAPPLE (Hak5)" onclick="addFilterCard(this)">+ Add selected to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Pentest / Hacking Tool (rogue access point)</div>
                    <div class="oui-meta"><strong>Frame Types:</strong> Beacon, Probe Request</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Hak5 WiFi Pineapple (address prefix community-reported)</div>
                    </details>
                    <details>
                    <summary><b>PWNAGOTCHI</b> <code>1 OUI</code></summary>
                    <div class="oui-entries"><code data-oui="DE:AD:BE" onclick="toggleOuiChip(this)">DE:AD:BE</code></div>
                    <button type="button" class="oui-add-btn" data-ble="false" data-wifi="true" data-label="PWNAGOTCHI" onclick="addFilterCard(this)">+ Add selected to filter list</button>
                    <div class="oui-meta"><strong>Category:</strong> Pentest / Hacking Tool (WiFi handshake capture)</div>
                    <div class="oui-meta"><strong>Frame Types:</strong> Beacon</div>
                    <div class="oui-meta"><strong>Common Devices:</strong> Pwnagotchi (beacons from DE:AD:BE:EF:DE:AD)</div>
                    </details>
                    <!-- WIFI_OUI_DB_END -->
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
                    <input type="text" id="ap_ssid" name="ap_ssid" value="%AP_SSID%" maxlength="32" autocomplete="off" class="field">
                    <div class="help-text" style="margin-top: 5px;">1-32 characters</div>
                </div>
                <div>
                    <label for="ap_password" style="display: block; margin-bottom: 8px; font-weight: 500; color: #ffffff;">Password</label>
                    <input type="text" id="ap_password" name="ap_password" value="%AP_PASSWORD%" minlength="8" maxlength="63" autocomplete="new-password" class="field">
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
                <div id="previousSessionPanel" style="display: none; margin-bottom: 15px; border: 1px solid rgba(255,255,255,0.12); border-radius: 8px; overflow: hidden; background: rgba(255,255,255,0.015);">
                    <div style="display:flex; align-items:center; justify-content:space-between; padding: 10px 14px; background: rgba(255,255,255,0.04); cursor: pointer;" onclick="togglePrevSession()">
                        <span id="previousSessionTitle" style="font-family:'Courier New',monospace; font-size:12px; letter-spacing:1px; color:#4ecdc4;">PREVIOUS SESSION (0)</span>
                        <span style="display:flex; align-items:center; gap:10px;">
                            <button type="button" onclick="event.stopPropagation(); clearPreviousSession();" style="background:#4a0000; padding:4px 10px; font-size:11px; margin:0;">Clear</button>
                            <span id="previousSessionCaret" style="color:#888; font-size:11px;">[-]</span>
                        </span>
                    </div>
                    <div id="previousSessionList" class="device-list" style="opacity: 0.65; padding: 10px 12px; max-height: 300px;"></div>
                </div>
                <div id="deviceList" class="device-list">
                    <div style="text-align: center; padding: 30px; color: #888888;">
                        <p style="font-size: 14px;">No device records in storage.</p>
                        <p style="font-size: 12px; margin-top: 10px;">Detected devices during scanning operations will persist to this list.</p>
                    </div>
                </div>
            </div>

            <details class="section mqtt-panel">
                <summary><h3>MQTT</h3><span class="mqtt-state">%MQTT_STATE%</span></summary>
                <div class="help-text" style="margin:12px 0 15px">Publish detections to MQTT broker on your network.</div>
                <div class="toggle-item" style="margin-bottom:15px">
                    <input type="checkbox" id="mqtt_en" name="mqtt_en" %MQTT_EN%>
                    <label class="toggle-label" for="mqtt_en">Enable MQTT</label>
                </div>
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">Device ID</label>
                <input type="text" name="mq_id" value="%MQ_ID%" maxlength="32" placeholder="ouispy" class="field">
                <div class="help-text" style="margin-bottom:12px">Unique name for this device (e.g. ouispy-front, ouispy-garage). Used as MQTT client ID and auto-generates topic.</div>
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">WiFi SSID</label>
                <input type="text" name="mq_ss" value="%MQ_SS%" maxlength="32" placeholder="Home WiFi" autocomplete="off" class="field">
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">WiFi Password</label>
                <input type="password" name="mq_sp" value="%MQ_SP%" maxlength="63" placeholder="Password" autocomplete="new-password" class="field">
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">Broker IP</label>
                <input type="text" name="mq_bk" value="%MQ_BK%" maxlength="64" placeholder="192.168.1.100" class="field">
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">Port</label>
                <input type="number" name="mq_pt" value="%MQ_PT%" min="1" max="65535" class="field">
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">Username (optional)</label>
                <input type="text" name="mq_us" value="%MQ_US%" maxlength="64" placeholder="Leave blank if none" class="field">
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">Password (optional)</label>
                <input type="password" name="mq_pw" value="%MQ_PW%" maxlength="64" placeholder="Leave blank if none" autocomplete="new-password" class="field">
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">Topic</label>
                <input type="text" name="mq_tp" value="%MQ_TP%" maxlength="128" placeholder="ouispy/detection" class="field">
            </details>

            <div class="section">
                <h3>Node Mesh (ESP-NOW Long Range)</h3>
                <div class="help-text" style="margin-bottom:15px">
                    Link multiple OUI-SPY nodes over ESP-NOW LR (hundreds of meters to ~1 km line-of-sight). Remote nodes flood their detections mesh-wide; the home node publishes everything it hears to MQTT. Detections relay automatically — nodes only need radio reach to <em>some</em> other node, not to the home node.
                </div>
                <div class="toggle-item" style="margin-bottom:15px">
                    <input type="checkbox" id="mesh_en" name="mesh_en" %MESH_EN%>
                    <label class="toggle-label" for="mesh_en">Enable Mesh</label>
                </div>
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">Node Role</label>
                <select name="mesh_role" class="field">
                    <option value="remote">Remote node — detect locally, uplink over mesh</option>
                    <option value="home" %MESH_HOME_SEL%>Home node — aggregate mesh + publish MQTT</option>
                </select>
                <div class="help-text" style="margin-bottom:12px">Exactly one home node should have MQTT configured — it is the mesh's gateway. Remote nodes ignore MQTT settings while meshing.</div>
                <label style="display:block;margin-bottom:4px;font-weight:500;color:#fff">Mesh Channel</label>
                <input type="number" name="mesh_ch" value="%MESH_CH%" min="1" max="11" class="field">
                <div class="help-text">WiFi channel all nodes share (1–11). Must match on every node. The radio parks here between scans for mesh traffic; remote nodes with no filters at all become pure relays.</div>
                <label style="display:block;margin:12px 0 4px;font-weight:500;color:#fff">Fleet Key</label>
                <input type="password" name="mesh_key" value="" minlength="8" maxlength="63" placeholder="%MESH_KEY_PH%" autocomplete="new-password" class="field">
                <div class="toggle-item" style="margin:6px 0">
                    <input type="checkbox" id="mesh_key_clear" name="mesh_key_clear">
                    <label class="toggle-label" for="mesh_key_clear">Clear fleet key</label>
                </div>
                <div class="help-text">Shared secret (8–63 characters) that signs fleet commands: mute, filter push, reboot, re-channel. Set the same key on every node; a node with a key only obeys commands signed with it, and replayed commands are refused. Leave blank to keep the current key.</div>
            </div>

            %MESH_MGMT%

            <div class="button-container">
                <button type="submit">Save Configuration & Start Scanning</button>
                <button type="button" onclick="clearConfig()" style="background: #8b0000; margin-left: 20px;">Clear All Filters</button>
                <button type="button" onclick="deviceReset()" style="background: #4a0000; margin-left: 20px; font-size: 12px;">Device Reset</button>
            </div>
            
            <!-- Burn In: collapsed expandable — the button IS the save -->
            <details class="burnin">
                <summary>Lock Config</summary>
                <div class="burnin-body">
                    <p class="burnin-warn"><strong>Permanent lock.</strong> Saves every setting on this page — filters, audio, AP, MQTT, node mesh — then locks the device. It boots straight into scanning; the portal never comes back on its own.</p>
                    <ul>
                        <li>WiFi AP and config mode disabled on boot</li>
                        <li>Boots directly to scanning in ~2 seconds</li>
                        <li>Unlock: hold the BOOT button during power-on (or erase flash + reflash)</li>
                    </ul>
                    <button type="button" class="burnin-btn" onclick="burnInConfig()">Save Settings &amp; Lock Config</button>
                </div>
            </details>
                        <style>
                .device-list { display: flex; flex-direction: column; gap: 6px; max-height: 400px; overflow-y: auto; }
                .device-item {
                    display: flex; flex-direction: column; gap: 8px; padding: 10px;
                    border: 1px solid var(--line); border-radius: 8px; background: var(--panel-2);
                }
                .device-info-row { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; }
                .device-alias-row { display: flex; align-items: center; gap: 10px; width: 100%%; }
                .device-mac { font-family: var(--mono); font-weight: 500; color: var(--accent); font-size: 13px; }
                .device-rssi { color: var(--muted); font-size: 12px; }
                .device-time { color: var(--faint); font-size: 11px; font-style: italic; }
                .device-time.recent { color: var(--accent); }
                .alias-input {
                    flex: 1; padding: 8px 12px; border: 1px solid var(--line-strong); border-radius: 6px;
                    background: #090c11; color: var(--text); font-size: 13px; min-width: 0; margin-bottom: 0;
                }
                .alias-input:focus { outline: none; border-color: var(--accent); box-shadow: 0 0 0 2px var(--accent-dim); }
                .save-alias-btn { padding: 8px 16px; font-size: 13px; margin: 0; white-space: nowrap; }
                .device-filter { color: var(--muted); font-size: 11px; font-style: italic; }
                .match-badge {
                    display: inline-block; padding: 2px 7px; border-radius: 5px;
                    font-family: var(--mono); font-size: 10px; font-weight: 700; letter-spacing: 0.5px;
                    background: rgba(0, 0, 0, 0.35); border: 1px solid currentColor;
                }
                .match-badge.type-OUI  { color: #7dd3fc; }
                .match-badge.type-MAC  { color: #e2e8f0; }
                .match-badge.type-CID  { color: var(--amber); }
                .match-badge.type-SVC  { color: #5eead4; }
                .match-badge.type-NAME { color: #c4b5fd; }
                .match-badge.type-META { color: #f0abfc; }
                .match-badge.type-PROBE  { background: rgba(78, 205, 196, 0.12); color: var(--accent); border-color: rgba(78, 205, 196, 0.30); }
                .match-badge.type-BEACON { background: rgba(78, 205, 196, 0.12); color: var(--accent); border-color: rgba(78, 205, 196, 0.30); }
                .prev-tag {
                    display: inline-block; margin-left: 6px; padding: 1px 6px; border-radius: 4px;
                    font-family: var(--mono); font-size: 9px; letter-spacing: 0.5px;
                    color: var(--faint); border: 1px solid var(--line); background: rgba(148, 163, 184, 0.04);
                }
                .match-badge.type-BLE { color: #7dd3fc; }
                .match-badge.type-WiFi { color: var(--accent); }

                .filter-row {
                    display: flex; flex-wrap: wrap; align-items: center; gap: 8px;
                    padding: 8px 10px; margin-bottom: 5px;
                    border: 1px solid var(--line); border-radius: 8px;
                    background: var(--panel-2); font-size: 13px;
                }
                .filter-domains { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; }
                .filter-domains .match-badge { cursor: pointer; min-height: 44px; display: inline-flex; align-items: center; }
                .filter-domains .match-badge:hover { opacity: 0.7; }
                .match-badge.disabled { color: var(--faint); border-color: var(--faint); opacity: 0.5; }
                .remove-filter-btn {
                    background: rgba(148, 163, 184, 0.06); color: var(--muted);
                    border: 1px solid var(--line-strong); border-radius: 6px;
                    padding: 6px 12px; cursor: pointer; font-size: 14px; line-height: 1;
                    min-height: 44px; min-width: 44px;
                    display: inline-flex; align-items: center; justify-content: center;
                }
                .remove-filter-btn:hover { background: rgba(148, 163, 184, 0.12); color: var(--text); filter: none; transform: none; box-shadow: none; }
                .preset-only { opacity: 0.55; }
                .empty-state-message { text-align: center; padding: 30px; color: var(--faint); font-size: 14px; }

                /* Filter list: a real list, not floating rows */
                #filterList {
                    border: 1px solid var(--line);
                    border-radius: 8px;
                    background: var(--panel-2);
                    overflow: hidden;
                    margin-bottom: 6px;
                }
                #filterList .filter-row {
                    display: grid;
                    grid-template-columns: minmax(96px, max-content) auto 1fr auto;
                    align-items: center;
                    gap: 12px;
                    padding: 8px 12px;
                    margin-bottom: 0;
                    border: none;
                    border-radius: 0;
                    background: transparent;
                }
                #filterList .filter-row + .filter-row { border-top: 1px solid var(--line); }
                #filterList .filter-row:hover { background: rgba(148, 163, 184, 0.04); }
                #filterList .device-mac { font-family: var(--mono); color: var(--accent); font-size: 13px; font-weight: 600; }
                #filterList .device-filter {
                    color: var(--muted); font-size: 12px; font-style: normal;
                    text-align: right; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
                }
                #filterList .filter-domains .match-badge { min-height: 0; padding: 2px 7px; }
                #filterList .remove-filter-btn { min-height: 26px; min-width: 26px; padding: 2px 7px; font-size: 13px; }
                #filterList .empty-state-message { padding: 18px; font-size: 13px; }

                /* Burn-in: collapsed danger expandable, button = save + lock */
                .burnin { border: 1px solid rgba(248, 113, 113, 0.25); border-radius: 10px; background: var(--panel); margin-top: 14px; overflow: hidden; }
                .burnin summary { cursor: pointer; padding: 13px 16px; font-size: 12px; font-weight: 700; letter-spacing: 2px; text-transform: uppercase; color: var(--red); list-style: none; text-align: center; }
                .burnin summary::-webkit-details-marker { display: none; }
                .burnin summary::before { content: "\25B8"; margin-right: 10px; font-size: 10px; }
                .burnin[open] summary::before { content: "\25BE"; }
                .burnin summary:hover { background: rgba(248, 113, 113, 0.05); }
                /* MQTT: collapsed by default, summary shows on/off */
                .mqtt-panel summary { cursor: pointer; list-style: none; display: flex; align-items: center; gap: 10px; }
                .mqtt-panel summary::-webkit-details-marker { display: none; }
                .mqtt-panel summary::before { content: "\25B8"; font-size: 11px; color: var(--muted); }
                .mqtt-panel[open] summary::before { content: "\25BE"; }
                .mqtt-panel summary h3 { margin: 0; }
                .mqtt-state { margin-left: auto; font-size: 11px; font-weight: 700; letter-spacing: 1.5px; color: var(--muted); }
                .burnin-body { padding: 2px 16px 16px; }
                .burnin-warn { font-size: 13px; line-height: 1.5; margin: 0 0 10px; }
                .burnin-warn strong { color: var(--red); }
                .burnin-body ul { margin: 0 0 14px; padding-left: 18px; color: var(--muted); font-size: 12px; line-height: 1.7; }
                .burnin-btn { background: rgba(248, 113, 113, 0.10) !important; color: #f87171 !important; border: 1px solid rgba(248, 113, 113, 0.40) !important; text-transform: uppercase; letter-spacing: 1px; font-size: 13px !important; font-weight: 700 !important; padding: 11px 22px !important; }
                .burnin-btn:hover { background: rgba(248, 113, 113, 0.20) !important; }

                /* Add-filter bar: manual entry composed with the list */
                .add-bar { display: flex; align-items: center; gap: 8px; margin-bottom: 10px; }
                .add-bar .field { flex: 1; margin-bottom: 0; }
                .add-bar .add-btn { margin: 0; }
                #addDomains .match-badge { min-height: 0; padding: 7px 10px; cursor: pointer; }

                /* Vendor-card OUI chips: selectable, grey when deselected */
                .oui-db .oui-entries code[data-oui] { cursor: pointer; user-select: none; -webkit-user-select: none; -webkit-tap-highlight-color: transparent; padding: 4px 10px; }
                .oui-db .oui-entries code[data-oui]:active { border-color: rgba(78, 205, 196, 0.45); }
                .oui-db .oui-entries code[data-oui].off { color: var(--faint); background: transparent; border-color: var(--line); }

                /* Mobile: real tap targets, sensible wrapping */
                @media (max-width: 560px) {
                    .add-bar { flex-wrap: wrap; }
                    .add-bar .field { flex: 1 1 100%; }
                    .add-bar .filter-domains { margin-left: auto; }
                    .add-bar .add-btn { margin: 0; }
                    #addDomains .match-badge { padding: 9px 14px; }
                    #filterList .filter-row { grid-template-columns: auto 1fr auto; }
                    #filterList .device-filter { grid-column: 1 / -1; text-align: left; }
                    button { min-height: 44px; }
                    .filter-domains .match-badge { min-height: 44px; padding: 4px 10px; }
                    .remove-filter-btn { min-height: 44px; min-width: 44px; }
                }
            </style>
            
            <script>
            // Load detected devices on page load
            window.addEventListener('DOMContentLoaded', function() {
                loadDetectedDevices();
                loadPreviousSession();
                
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

            function togglePrevSession() {
                var list = document.getElementById('previousSessionList');
                var caret = document.getElementById('previousSessionCaret');
                if (!list) return;
                if (list.style.display === 'none') {
                    list.style.display = '';
                    caret.textContent = '[-]';
                } else {
                    list.style.display = 'none';
                    caret.textContent = '[+]';
                }
            }

            function loadPreviousSession() {
                fetch('/api/session/previous')
                    .then(function(r) { return r.json(); })
                    .then(function(arr) {
                        var panel = document.getElementById('previousSessionPanel');
                        var list  = document.getElementById('previousSessionList');
                        var title = document.getElementById('previousSessionTitle');
                        if (!Array.isArray(arr) || arr.length === 0) {
                            if (panel) panel.style.display = 'none';
                            return;
                        }
                        panel.style.display = '';
                        title.textContent = 'PREVIOUS SESSION (' + arr.length + ')';
                        list.innerHTML = '';
                        arr.forEach(function(entry) {
                            var item = document.createElement('div');
                            item.className = 'device-item';
                            var row = document.createElement('div');
                            row.className = 'device-info-row';

                            var macSpan = document.createElement('span');
                            macSpan.className = 'device-mac';
                            macSpan.textContent = entry.mac || '?';
                            row.appendChild(macSpan);

                            row.appendChild(makeMatchBadge(entry.type, entry.desc, entry.match));

                            if (typeof entry.rssi === 'number') {
                                var rssi = document.createElement('span');
                                rssi.className = 'device-rssi';
                                rssi.textContent = entry.rssi + ' dBm';
                                row.appendChild(rssi);
                            }

                            var prevTag = document.createElement('span');
                            prevTag.className = 'prev-tag';
                            prevTag.textContent = 'PREV';
                            row.appendChild(prevTag);

                            if (entry.desc) {
                                var descSpan = document.createElement('span');
                                descSpan.className = 'device-filter';
                                descSpan.textContent = entry.desc;
                                descSpan.title = entry.desc;
                                row.appendChild(descSpan);
                            }
                            item.appendChild(row);
                            list.appendChild(item);
                        });
                    })
                    .catch(function(err) {
                        console.error('prev session load failed', err);
                    });
            }

            function clearPreviousSession() {
                fetch('/api/session/clear_previous', { method: 'POST' })
                    .then(function() {
                        var panel = document.getElementById('previousSessionPanel');
                        if (panel) panel.style.display = 'none';
                    })
                    .catch(function(err) { console.error(err); });
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
                    button.style.background = 'linear-gradient(135deg, #10b981 0%%%, #059669 100%%%)';
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
                    button.style.background = 'linear-gradient(135deg, #ef4444 0%%%, #dc2626 100%%%)';
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
                verkada:'E0:A7:00',
                meta:   '7C:2A:9E,CC:66:0A,F4:03:43,5C:E9:1E,98:59:49'
            };
            var VENDOR_LABELS = { axon: 'AXON', meta: 'META / RAY-BAN', ipro: 'I-PRO', verkada: 'VERKADA' };

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
                    // WiFi-vendor cards: wifi domain only. Installing a BLE
                    // row for a WiFi-only bodycam reads wrong and rides along
                    // in every mesh filter push.
                    addFilterRow(ouiStr.replace(':','').toUpperCase(), false, true, label);
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
            
            var _burnArm = 0;
            function burnInConfig() {
                // Two-tap arm/confirm — native confirm() is silently dropped
                // by captive-portal webviews (iOS especially), which made
                // the button appear dead.
                var now = Date.now();
                if (now - _burnArm > 4000) {
                    _burnArm = now;
                    var b = document.querySelector('.burnin-btn');
                    if (b) {
                        b.textContent = 'CONFIRM LOCK — TAP AGAIN';
                        b.style.background = 'rgba(248,113,113,0.28)';
                    }
                    setTimeout(function() {
                        var bb = document.querySelector('.burnin-btn');
                        if (bb && Date.now() - _burnArm >= 4000) {
                            bb.textContent = 'Save Settings & Lock Config';
                            bb.style.background = '';
                        }
                    }, 4200);
                    return;
                }
                _burnArm = 0;
                buildFilterList();
                var f = document.getElementById('lockForm');
                if (!f) return;
                var cfg = document.getElementById('configForm');
                function set(name, val) {
                    var el = f.elements[name];
                    if (!el) return;
                    if (val === null || val === undefined) { el.disabled = true; }
                    else { el.disabled = false; el.value = val; }
                }
                function valById(id) { var el = document.getElementById(id); return el ? el.value : ''; }
                function checked(id) { var el = document.getElementById(id); return !!(el && el.checked); }
                function valByName(name) {
                    var el = cfg ? cfg.querySelector('[name=' + name + ']') : null;
                    return el ? el.value : '';
                }

                set('filters', (document.getElementById('filtersJson') || {}).value || '[]');
                set('buzzerEnabled', checked('buzzerEnabled') ? 'on' : null);
                set('ledEnabled', checked('ledEnabled') ? 'on' : null);
                set('ap_ssid', valById('ap_ssid'));
                set('ap_password', valById('ap_password'));
                set('mqtt_en', checked('mqtt_en') ? 'on' : null);
                ['mq_id','mq_ss','mq_sp','mq_bk','mq_pt','mq_us','mq_pw','mq_tp'].forEach(function(k){ set(k, valByName(k)); });
                set('mesh_en', checked('mesh_en') ? 'on' : null);
                set('mesh_role', valByName('mesh_role'));
                set('mesh_ch', valByName('mesh_ch'));
                set('mesh_key', valByName('mesh_key'));
                set('mesh_key_clear', checked('mesh_key_clear') ? 'on' : null);
                f.submit();
            }

            // === Filter List Management ===

            var dirtyState = false;
            var _batchAdd = false;

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

            function findRowDescByOui(oui) {
                var list = document.getElementById('filterList');
                if (!list) return '';
                var rows = list.querySelectorAll('.filter-row');
                for (var i = 0; i < rows.length; i++) {
                    if (rows[i].getAttribute('data-oui') === oui) {
                        return rows[i].getAttribute('data-desc') || '';
                    }
                }
                return '';
            }

            function toggleOuiChip(el) {
                // Per-OUI selection inside vendor cards: click to deselect
                // (greys out); addFilterCard skips deselected chips.
                el.classList.toggle('off');
            }

            function addFilterCard(btn) {
                var card = btn.closest('details');
                if (!card) return;
                if (!checkDirtyBeforePreset()) return;
                var ble = btn.getAttribute('data-ble') === 'true';
                var wifi = btn.getAttribute('data-wifi') === 'true';
                var label = btn.getAttribute('data-label') || '';
                var chips = card.querySelectorAll('.oui-entries code[data-oui]');
                var batch = [];
                for (var i = 0; i < chips.length; i++) {
                    if (chips[i].classList.contains('off')) continue;
                    batch.push([chips[i].getAttribute('data-oui').replace(/:/g, '').toUpperCase(), ble, wifi, label]);
                }
                if (batch.length === 0) { alert('Every OUI in this card is deselected.'); return; }
                _batchAdd = true;
                for (var j = 0; j < batch.length; j++) {
                    addFilterRow(batch[j][0], batch[j][1], batch[j][2], batch[j][3]);
                }
                _batchAdd = false;
                updateEmptyState();
            }

            function addFilterBatch(rows) {
                // Vendor multi-add: ONE dirty-check for the whole batch —
                // per-row checks would re-prompt after the first row sets
                // dirtyState and silently truncate the add.
                if (!checkDirtyBeforePreset()) return;
                _batchAdd = true;
                for (var i = 0; i < rows.length; i++) {
                    addFilterRow(rows[i][0], rows[i][1], rows[i][2], rows[i][3]);
                }
                _batchAdd = false;
                updateEmptyState();
            }

            function addFilterRow(oui, ble, wifi, desc) {
                if (!_batchAdd && !checkDirtyBeforePreset()) return;

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
                        alert(formatMac(clean) + ' is already in the list (' + (exRow.getAttribute('data-desc') || 'custom filter') + ') with the same domains.');
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
                    // Recognize OUIs already known (preset or prior row) and
                    // label the new entry properly instead of generically.
                    desc = findRowDescByOui(clean);
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
                var doms = document.getElementById('addDomains');
                var ble = doms.getAttribute('data-ble') !== 'false';
                var wifi = doms.getAttribute('data-wifi') !== 'false';
                if (!ble && !wifi) {
                    alert('Pick at least one domain: BLE or WiFi.');
                    return;
                }
                addFilterRow(clean, ble, wifi, '');
                input.value = '';
                input.focus();
            }

            function toggleAddDomain(el) {
                var box = document.getElementById('addDomains');
                var dom = el.getAttribute('data-dom');
                var cur = box.getAttribute('data-' + dom) !== 'false';
                box.setAttribute('data-' + dom, cur ? 'false' : 'true');
                el.classList.toggle('disabled', cur);
                setDirty();
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
                        // WiFi subtype carried through the save so beacon
                        // filters aren't degraded to probe (server default).
                        wtype: r.getAttribute('data-wtype') || '',
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
        <p style="margin:18px 4px 6px;font-size:10px;line-height:1.5;opacity:0.55;text-align:center;">
            For research purposes only. Use at your own risk: you assume all responsibility for how you use it, and the authors accept no liability. Provided as is, with no warranty. Follow your local laws, and don't do anything illegal or stupid.
        </p>
    </div>
</body>
</html>
)html";

// Mesh Management panel — injected into the config page via %MESH_MGMT% on
// home nodes only. Served as processor output, so (unlike CONFIG_HTML) no
// %-escaping is needed here.
const char MESH_MGMT_HTML[] PROGMEM = R"meshmgmt(
            <div class="section" style="border:1px solid rgba(78,205,196,0.35)">
                <h3 style="color:#4ecdc4">Mesh Management</h3>
                <div class="help-text" style="margin-bottom:12px">
                    Remote fleet control. Commands flood mesh-wide and relay onward; the ack count shows how many remotes in direct reach confirmed.
                </div>
                <div id="meshNodes" style="font-family:'Courier New',monospace;font-size:13px;margin-bottom:12px">querying mesh…</div>
                <div style="display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:10px">
                    <button type="button" onclick="meshAudio(0)" style="background:rgba(251,191,36,.12);color:#fbbf24;border:1px solid rgba(251,191,36,.35)">Mute All Remotes</button>
                    <button type="button" onclick="meshAudio(1)">Unmute All Remotes</button>
                    <span style="font-size:13px;color:#8593a6"><input type="checkbox" id="meshMuteLed"> also LED</span>
                </div>
                <div style="display:flex;gap:10px;flex-wrap:wrap;margin-bottom:10px">
                    <button type="button" onclick="meshPushFilters()">Push Current Filters to Mesh</button>
                    <button type="button" onclick="meshRebootAll()" style="background:rgba(248,113,113,.10);color:#f87171;border:1px solid rgba(248,113,113,.35)">Reboot All Remotes</button>
                </div>
                <div id="meshStatus" style="font-size:13px;color:#4ecdc4;min-height:18px"></div>
            </div>
            <script>
            (function(){
                var ackTimer=null;
                function status(m){document.getElementById('meshStatus').innerHTML=m;}
                window.meshStatusMsg=status;
                function watchAcks(cmd){
                    if(ackTimer)clearInterval(ackTimer);
                    ackTimer=setInterval(function(){
                        fetch('/api/mesh/acks?cmd='+cmd).then(function(r){return r.json();}).then(function(d){
                            var el=document.getElementById('meshAckCount');
                            if(el)el.innerHTML=d.count;
                        }).catch(function(){});
                    },1500);
                }
                window.meshWatchAcks=watchAcks;
                window.meshRefresh=function(){
                    fetch('/api/mesh/nodes').then(function(r){return r.json();}).then(function(d){
                        var el=document.getElementById('meshNodes');
                        if(!el)return;
                        if(!d.nodes.length){el.innerHTML='<span style="color:#888">no remote nodes heard yet</span>';return;}
                        var h='<table style="width:100%;border-collapse:collapse;color:#ddd">';
                        h+='<tr style="color:#888"><td style="padding:2px 8px 2px 0">node</td><td style="padding:2px 8px">role</td><td style="padding:2px 8px">last seen</td><td style="padding:2px 8px">packets</td></tr>';
                        d.nodes.forEach(function(n){
                            h+='<tr><td style="padding:2px 8px 2px 0">'+n.id+'</td><td style="padding:2px 8px">'+n.role+'</td><td style="padding:2px 8px">'+n.last+'s ago</td><td style="padding:2px 8px">'+n.pkts+'</td></tr>';
                        });
                        el.innerHTML=h+'</table>';
                    }).catch(function(){
                        var el=document.getElementById('meshNodes');
                        if(el)el.innerHTML='<span style="color:#f66">mesh query failed</span>';
                    });
                };
                setInterval(window.meshRefresh,5000);
                if(document.getElementById('meshNodes'))window.meshRefresh();
                window.meshAudio=function(on){
                    var l=document.getElementById('meshMuteLed').checked?0:1;
                    if(!on && !confirm('Mute buzzer (and LED, if checked) on ALL remote nodes?'))return;
                    fetch('/api/mesh/audio?b='+(on?1:0)+'&l='+l,{method:'POST'}).then(function(r){return r.json();}).then(function(d){
                        if(!d.ok){status('command failed: '+(d.error||'unknown'));return;}
                        status((on?'unmute':'mute')+' flooded — acks: <b><span id="meshAckCount">0</span></b>');
                        watchAcks(d.cmd);
                    }).catch(function(){status('command failed (network)');});
                };
                window.meshRebootAll=function(){
                    if(!confirm('Reboot ALL remote nodes? They rejoin the mesh in a few seconds.'))return;
                    fetch('/api/mesh/reboot',{method:'POST'}).then(function(r){return r.json();}).then(function(d){
                        if(!d.ok){status('reboot failed: '+(d.error||'unknown'));return;}
                        status('reboot flooded — acks: <b><span id="meshAckCount">0</span></b>');
                        watchAcks(d.cmd);
                    }).catch(function(){status('reboot failed (network)');});
                };
                window.meshPushFilters=function(){
                    if(typeof buildFilterList==='function')buildFilterList();
                    var f=document.getElementById('filtersJson')?document.getElementById('filtersJson').value:'[]';
                    if(!confirm('Replace filters on ALL remote nodes with the list currently in this form? Remotes apply it live and keep scanning.'))return;
                    status('pushing filters…');
                    fetch('/api/mesh/push-filters',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'filters='+encodeURIComponent(f)})
                        .then(function(r){return r.json();}).then(function(d){
                            if(!d.ok){status('push failed: '+(d.error||'unknown'));return;}
                            status('pushed '+d.count+' filters — remotes rebooting; acks: <b><span id="meshAckCount">0</span></b>');
                            watchAcks(d.cmd);
                        }).catch(function(){status('push failed (network)');});
                };
            })();
            </script>
)meshmgmt";

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
        // WiFi probe and beacon filters never share a group: each gets its own
        // row carrying data-wtype, so a save→parse round-trip preserves both
        // (a merged row would degrade the beacon filter to probe on save).
        // Collect unique keys in order of first appearance.
        typedef struct { String key; String display; bool ble; bool wifi; String wtype; bool fullMac; String desc; } OuiGroup;
        std::vector<OuiGroup> groups;

        for (const TargetFilter& f : targetFilters) {
            if (f.type == FT_MAC_PREFIX || f.type == FT_WIFI_PROBE || f.type == FT_WIFI_BEACON) {
                // Group key = first 6 chars of identifier (the OUI)
                String key = f.identifier.substring(0, 6);
                key.toUpperCase();
                String wt = (f.type == FT_WIFI_PROBE)  ? "probe" :
                            (f.type == FT_WIFI_BEACON) ? "beacon" : "";

                // Find or create group
                bool found = false;
                for (auto& g : groups) {
                    if (g.key == key && g.wtype == wt) {
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
                    g.wtype = wt;
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
                    g.wtype = "";
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

            rows += "<div class=\"filter-row\" data-oui=\"" + escapedOui + "\" data-ble=\"" + bleAttr + "\" data-wifi=\"" + wifiAttr + "\" data-wtype=\"" + g.wtype + "\" data-desc=\"" + escapedDesc + "\">";
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
    if (var == "ASCII_ART") return String(getASCIIArt());
    if (var == "MQTT_EN") return mqttCfg.enabled ? "checked" : "";
    if (var == "MQTT_STATE") return mqttCfg.enabled ? "ON" : "OFF";
    if (var == "MQ_SS") return mqttCfg.sta_ssid;
    if (var == "MQ_SP") return mqttCfg.sta_pass;
    if (var == "MQ_BK") return mqttCfg.broker;
    if (var == "MQ_PT") return String(mqttCfg.port);
    if (var == "MQ_US") return mqttCfg.user;
    if (var == "MQ_PW") return mqttCfg.pass;
    if (var == "MQ_ID") return mqttCfg.device_id;
    if (var == "MQ_TP") return mqttCfg.topic;
    if (var == "MESH_EN") return meshCfg.enabled ? "checked" : "";
    if (var == "MESH_CH") return String(meshCfg.channel);
    if (var == "MESH_HOME_SEL") return meshCfg.home ? "selected" : "";
    if (var == "MESH_KEY_PH") return meshKeySet() ? "set (leave blank to keep)" : "not set: fleet commands are unauthenticated";
    if (var == "MESH_MGMT") {
        if (!meshCfg.enabled || !meshCfg.home) return "";
        return String(FPSTR(MESH_MGMT_HTML));
    }
    return String();
}

// Shared filter-JSON expansion, used by the /save handler and the mesh
// push-filters endpoint. Parses and validates the portal's filter JSON into
// `out` WITHOUT touching targetFilters — the caller commits the result only
// after a fully successful parse. Returns the expanded filter count, or -1
// on malformed JSON. Heap document (like rotateSessionFiles) — an 8 KB stack
// document inside the async-web handler task would be dicey, and a single
// Flock 30-OUI card click already posts ~3 KB of rows.
static int expandFiltersJson(const String& filtersJson, std::vector<TargetFilter>& out) {
    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, filtersJson);
    if (err.code() != DeserializationError::Ok) {
        if (isSerialConnected()) Serial.print("filters JSON parse error: ");
        if (isSerialConnected()) Serial.println(err.c_str());
        return -1;
    }
    if (!doc.is<JsonArray>()) return -1;

    int added = 0;
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
            out.push_back(f);
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
                out.push_back(f);
                added++;
            }
            if (wifi) {
                TargetFilter f;
                f.identifier = normalizedOUI;
                String formatted = normalizedOUI.substring(0,2) + ":" + normalizedOUI.substring(2,4) + ":" + normalizedOUI.substring(4,6);
                f.description = desc.isEmpty() ? "OUI: " + formatted + " (WiFi)" : desc;
                f.isFullMAC = false;
                // Filter subtype round-trips from the form ("probe" default,
                // "beacon" preserved) so a save without changes is a no-op.
                const char* wtype = entry["wtype"] | "probe";
                f.type = (strcmp(wtype, "beacon") == 0) ? FT_WIFI_BEACON : FT_WIFI_PROBE;
                out.push_back(f);
                added++;
            }
        }
    }
    return added;
}

// Parse the 'filters' JSON form field and rebuild targetFilters. Clears
// user-managed filter types (FT_MAC_PREFIX, FT_FULL_MAC, FT_WIFI_PROBE,
// FT_WIFI_BEACON) and preserves preset-only types. The POSTed JSON is parsed
// and fully validated BEFORE anything is erased, so a malformed or oversized
// payload leaves the stored configuration untouched. Returns the number of
// new filter entries added, or -1 if the posted filter list could not be
// parsed (caller must reject the save).
static int parseFiltersFromJSON(AsyncWebServerRequest *request) {
    if (!request->hasParam("filters", true)) {
        return 0;
    }
    String filtersJson = request->getParam("filters", true)->value();
    filtersJson.trim();
    if (filtersJson.length() == 0) return 0;

    std::vector<TargetFilter> expanded;
    int added = expandFiltersJson(filtersJson, expanded);
    if (added < 0) return -1;

    // Parse succeeded — only now is it safe to clear the user-managed filter
    // types (preset-only types are always preserved) and commit the result.
    targetFilters.erase(
        std::remove_if(targetFilters.begin(), targetFilters.end(),
            [](const TargetFilter& f) {
                return f.type == FT_MAC_PREFIX || f.type == FT_FULL_MAC ||
                       f.type == FT_WIFI_PROBE || f.type == FT_WIFI_BEACON;
            }),
        targetFilters.end());
    targetFilters.insert(targetFilters.end(), expanded.begin(), expanded.end());
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
    
    // Meshed home nodes manage the fleet from the portal, so the mesh must
    // coexist with the AP here. ESP-NOW + softAP is order-sensitive on this
    // core: the mesh comes up FIRST and the AP beacon engine starts LAST —
    // the reverse order leaves the driver reporting "AP up" while no client
    // can see the beacon.
    bool meshPortal = meshCfg.enabled && meshCfg.home;
    if (meshPortal) {
        Serial.println("Mesh home: portal AP will share the mesh channel " + String(meshCfg.channel));
    }

    // Ensure WiFi is off first
    WiFi.mode(WIFI_OFF);
    delay(1000);

    Serial.println("Setting WiFi mode...");
    WiFi.mode(meshPortal ? WIFI_AP_STA : WIFI_AP);
    delay(500);

    // Bring the mesh up FIRST (order matters — see above), then start the
    // AP so its beacon engine initializes on top of a running ESP-NOW.
    if (meshPortal) {
        mesh_init();
        // A heal sweep would drag the shared radio - and the AP with it -
        // across channels while the portal is up.
        meshSetHealAllowed(false);
    }
    delay(300);

    Serial.println("Creating access point...");
    // Never broadcast open: a cleared/blank stored credential falls back to
    // the factory secured values.
    String apSsid = AP_SSID.length() > 0 ? AP_SSID : "snoopuntothem";
    String apPass = AP_PASSWORD.length() > 0 ? AP_PASSWORD : "astheysnoopuntous";
    // The portal AP must land on a universally visible channel.
    uint8_t apCh = meshPortal ? meshCfg.channel : 1;
    if (apCh > 11) apCh = 1;
    bool apStarted = WiFi.softAP(apSsid.c_str(), apPass.c_str(), apCh);
    if (!apStarted) {
        Serial.println("✗ Failed to create Access Point!");
        return;
    }
    Serial.println("✓ Access Point created successfully!");
    apForcePlainProtocol();
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

        // no-store: phones (especially the iOS captive-portal sheet) cache
        // the portal aggressively — a stale copy is exactly what makes
        // "the button does nothing" after a firmware update.
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", CONFIG_HTML, configProcessor);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });
    
    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {

        if (isSerialConnected()) {
            Serial.println("\n=== WEB CONFIG SUBMISSION ===");
        }

        // Parse filters from JSON array (new filter UI format)
        int filterCount = parseFiltersFromJSON(request);
        if (filterCount < 0) {
            // Parse failed BEFORE anything was erased — stored config is
            // untouched. Reject the save so it can't pretend to succeed.
            request->send(400, "text/html",
                "<!DOCTYPE html><html><head><title>Save failed</title></head><body style='font-family:sans-serif;background:#1a1a1a;color:#eee'>"
                "<h1>Could not save</h1><p>The filter list failed to parse (too large or malformed). "
                "Your saved configuration was <b>not</b> changed. Go back and retry.</p></body></html>");
            return;
        }

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

        // Process node mesh settings
        meshCfg.enabled = request->hasParam("mesh_en", true);
        if (request->hasParam("mesh_role", true)) {
            meshCfg.home = request->getParam("mesh_role", true)->value() == "home";
        }
        if (request->hasParam("mesh_ch", true)) {
            int ch = request->getParam("mesh_ch", true)->value().toInt();
            if (ch >= 1 && ch <= 11) meshCfg.channel = (uint8_t)ch;
        }
        if (request->hasParam("mesh_key_clear", true)) {
            meshSetKey("");
        } else if (request->hasParam("mesh_key", true)) {
            String k = request->getParam("mesh_key", true)->value();
            k.trim();
            if (k.length() >= 8 && k.length() <= 63) meshSetKey(k.c_str());
        }
        mesh_saveConfig();

        if (isSerialConnected()) {
            Serial.println("Buzzer enabled: " + String(buzzerEnabled ? "Yes" : "No"));
            Serial.println("LED enabled: " + String(ledEnabled ? "Yes" : "No"));
            Serial.println("WiFi SSID: " + AP_SSID);
            Serial.println("WiFi Password: " + String(AP_PASSWORD.length() > 0 ? "********" : "(Open Network)"));
            Serial.println("Node Mesh: " + String(meshCfg.enabled ? (meshCfg.home ? "home" : "remote") : "disabled") +
                           (meshCfg.enabled ? " ch" + String(meshCfg.channel) : ""));
        }
        
        // One-shot save-and-lock: the LOCK CONFIG button posts here with
        // burnin=1 — same saves, same success screen, plus the lock.
        bool burnin = request->hasParam("burnin", true);

        if (targetFilters.size() > 0) {
            saveConfiguration();
            // Lock only once the save is known good: a rejected (empty)
            // save must never leave a locked node with nothing to scan for.
            if (burnin) {
                preferences.begin("ouispy", false);
                preferences.putBool("configLocked", true);
                preferences.end();
                if (isSerialConnected()) Serial.println("BURN-IN: configuration locked via portal save");
            }

            if (isSerialConnected()) {
                Serial.println("Saved " + String(targetFilters.size()) + " filters:");
                for (const TargetFilter& filter : targetFilters) {
                    String type = filter.isFullMAC ? "Full MAC" : "OUI";
                    Serial.println("  - " + filter.identifier + " (" + type + ")");
                }
            }
            
            // Informational hand-off screen before the switch to scanning:
            // what was saved, how the node will behave, and - for burn-in -
            // what the lock means and how to undo it. Plain send (no
            // template processor), so a literal % is fine here.
            auto esc = [](const String& in) {
                String o; o.reserve(in.length() + 8);
                for (size_t k = 0; k < in.length(); k++) {
                    char c = in[k];
                    if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;";
                    else if (c == '&') o += "&amp;"; else if (c == '"') o += "&quot;";
                    else o += c;
                }
                return o;
            };
            String rows;
            size_t shown = 0;
            for (const TargetFilter& f : targetFilters) {
                if (shown++ >= 40) { rows += "<li class='more'>+ " + String(targetFilters.size() - 40) + " more</li>"; break; }
                rows += "<li><code>" + esc(f.identifier) + "</code>";
                if (f.description.length()) rows += "<span>" + esc(f.description) + "</span>";
                rows += "</li>";
            }
            String meshLine = meshCfg.enabled
                ? "Node mesh <b>on</b> &middot; role <b>" + String(meshCfg.home ? "HOME" : "REMOTE") +
                  "</b> &middot; channel <b>" + String(meshCfg.channel) + "</b>"
                : String("Node mesh <b>off</b> &middot; standalone detector");
            String burnBlock;
            if (burnin) {
                burnBlock =
                    "<div class='burn'><h2>Burned in &mdash; configuration locked</h2><ul>"
                    "<li>From now on this node <b>boots straight into scanning</b>. The setup portal and its WiFi network will not come back on their own.</li>"
                    "<li>" + String(meshCfg.enabled
                        ? "It joins the mesh on channel " + String(meshCfg.channel) + " as a " + String(meshCfg.home ? "home" : "remote") +
                          " node. A home node can still push filters, audio settings, re-channel and reboot it over the mesh."
                        : String("The mesh is off, so it runs standalone: filters and settings are fixed until it is unlocked.")) + "</li>"
                    "<li><b>To unlock:</b> hold the BOOT button while powering on and keep holding about 1.5 s, until the triple beep. It comes back up in config mode with every setting intact.</li>"
                    "<li>Last resort: erase flash and reflash (wipes all settings).</li>"
                    "</ul></div>";
            }
            String responseHTML = String(R"html(<!DOCTYPE html>
<html>
<head>
    <title>)html") + (burnin ? "Burned In" : "Configuration Saved") + R"html(</title>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 0; padding: 20px; background: #0d0d10; color: #e0e0e0; }
        .container { max-width: 620px; margin: 0 auto; background: #1b1b22; padding: 28px 26px; border-radius: 14px; border: 1px solid #2c2c38; }
        h1 { color: #fff; margin: 0 0 18px; font-weight: 600; letter-spacing: 1px; text-align: center; }
        h2 { margin: 0 0 10px; font-size: 15px; letter-spacing: 1px; text-transform: uppercase; }
        .success { background: #12352a; color: #4ade80; border: 1px solid #166534; padding: 14px 16px; border-radius: 10px; text-align: center; }
        .success p { margin: 4px 0; }
        .card { background: #14141a; border: 1px solid #2c2c38; border-radius: 10px; padding: 14px 16px; margin: 14px 0; }
        .card h2 { color: #c084fc; }
        ul { margin: 0; padding-left: 18px; line-height: 1.6; font-size: 14px; }
        .filters { list-style: none; padding: 0; max-height: 260px; overflow-y: auto; }
        .filters li { display: flex; gap: 10px; align-items: baseline; padding: 5px 0; border-bottom: 1px solid #22222c; }
        .filters li:last-child { border-bottom: 0; }
        .filters code { color: #f0abfc; font-size: 13px; white-space: nowrap; }
        .filters span { color: #a1a1aa; font-size: 13px; }
        .filters .more { color: #a1a1aa; }
        .mesh { font-size: 14px; color: #d4d4d8; }
        .burn { background: rgba(248,113,113,0.08); border: 1px solid rgba(248,113,113,0.45); border-radius: 10px; padding: 14px 16px; margin: 14px 0; }
        .burn h2 { color: #f87171; }
        .burn li { margin: 6px 0; }
        .note { color: #a1a1aa; font-size: 13px; text-align: center; line-height: 1.6; }
    </style>
    <script>
        var left = 5;
        setInterval(function() {
            left--;
            var c = document.getElementById('countdown');
            if (c) c.innerHTML = left > 0 ? 'Switching to scanning mode in ' + left + ' s...' : 'Switching to scanning mode now...';
        }, 1000);
    </script>
</head>
<body>
    <div class="container">
        <h1>)html" + String(burnin ? "Burned In" : "Configuration Saved") + R"html(</h1>
        <div class="success">
            <p><strong>Saved )html" + String(targetFilters.size()) + R"html( filter)html" + String(targetFilters.size() == 1 ? "" : "s") + R"html(.</strong></p>
            <p id="countdown">Switching to scanning mode in 5 s...</p>
        </div>
)html" + burnBlock + R"html(
        <div class="card">
            <h2>Watching for</h2>
            <ul class="filters">)html" + rows + R"html(</ul>
        </div>
        <div class="card mesh">)html" + meshLine + R"html(</div>
        <p class="note">Every match triggers the buzzer and LED alerts (if enabled).<br>The WiFi portal shuts down when scanning starts &mdash; this page will lose its connection.</p>
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
            // Nothing to scan for: stay put (no lock, no mode switch) and
            // say so plainly instead of a bare error.
            request->send(400, "text/html", String(R"html(<!DOCTYPE html>
<html><head><title>No Filters Selected</title><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 0; padding: 20px; background: #0d0d10; color: #e0e0e0; }
.container { max-width: 560px; margin: 0 auto; background: #1b1b22; padding: 28px 26px; border-radius: 14px; border: 1px solid #2c2c38; text-align: center; }
h1 { color: #fbbf24; font-weight: 600; margin: 0 0 14px; }
p { line-height: 1.6; color: #d4d4d8; }
a { display: inline-block; margin-top: 16px; padding: 11px 22px; border-radius: 8px; background: #7c3aed; color: #fff; text-decoration: none; font-weight: 600; }
</style></head><body><div class="container">
<h1>No filters selected</h1>
<p>Nothing was saved)html") + (burnin ? " and the device was <b>not</b> locked" : "") + R"html(. Pick at least one filter &mdash; an OUI, a preset, or a WiFi/BLE signature &mdash; so the detector has something to scan for, then save again.</p>
<a href="/">Back to settings</a>
</div></body></html>)html");
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

    // Previous-session panel data source. Serves /session_prev.json
    // straight from LittleFS; the UI does its own rendering.
    server.on("/api/session/previous", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", readPreviousSessionJson());
    });

    // Clear the previous-session file so the panel disappears on the
    // next dashboard refresh. Does not touch the current session.
    server.on("/api/session/clear_previous", HTTP_POST, [](AsyncWebServerRequest *request) {
        clearPreviousSessionFile();
        request->send(200, "application/json", "{\"success\":true}");
    });

    // --- Mesh management API (home node with mesh enabled) ---
    // The portal's Mesh Management panel talks to these; remotes get a 403.

    server.on("/api/mesh/nodes", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json;
        meshNodesJson(json);
        request->send(200, "application/json", json);
    });

    server.on("/api/mesh/acks", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("cmd")) {
            request->send(400, "application/json", "{\"count\":0}");
            return;
        }
        uint16_t cmdId = (uint16_t)request->getParam("cmd")->value().toInt();
        request->send(200, "application/json",
                      "{\"count\":" + String(meshCmdAckCount(cmdId)) + "}");
    });

    server.on("/api/mesh/audio", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!meshCfg.enabled || !meshCfg.home) {
            request->send(403, "application/json", "{\"ok\":0,\"error\":\"home node with mesh enabled required\"}");
            return;
        }
        uint8_t p[2] = {1, 1};
        if (request->hasParam("b", true)) p[0] = request->getParam("b", true)->value().toInt() ? 1 : 0;
        if (request->hasParam("l", true)) p[1] = request->getParam("l", true)->value().toInt() ? 1 : 0;
        uint16_t cmdId = meshNewCmdId();
        if (!meshQueueCommand(MESH_CMD_AUDIO, cmdId, p, 2)) {
            request->send(500, "application/json", "{\"ok\":0,\"error\":\"mesh TX failed\"}");
            return;
        }
        request->send(200, "application/json", "{\"ok\":1,\"cmd\":" + String(cmdId) + "}");
    });

    server.on("/api/mesh/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!meshCfg.enabled || !meshCfg.home) {
            request->send(403, "application/json", "{\"ok\":0,\"error\":\"home node with mesh enabled required\"}");
            return;
        }
        uint16_t cmdId = meshNewCmdId();
        if (!meshQueueCommand(MESH_CMD_REBOOT, cmdId, NULL, 0)) {
            request->send(500, "application/json", "{\"ok\":0,\"error\":\"mesh TX failed\"}");
            return;
        }
        request->send(200, "application/json", "{\"ok\":1,\"cmd\":" + String(cmdId) + "}");
    });

    server.on("/api/mesh/push-filters", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!meshCfg.enabled || !meshCfg.home) {
            request->send(403, "application/json", "{\"ok\":0,\"error\":\"home node with mesh enabled required\"}");
            return;
        }
        if (!request->hasParam("filters", true)) {
            request->send(400, "application/json", "{\"ok\":0,\"error\":\"missing filters payload\"}");
            return;
        }
        String filtersJson = request->getParam("filters", true)->value();
        filtersJson.trim();
        std::vector<TargetFilter> expanded;
        if (expandFiltersJson(filtersJson, expanded) < 0) {
            request->send(400, "application/json", "{\"ok\":0,\"error\":\"invalid filter JSON\"}");
            return;
        }

        // Serialize to the compact binary record format remotes write to NVS.
        // Heap buffer — same reason expandFiltersJson uses a heap document.
        uint8_t* buf = (uint8_t*)malloc(4096);
        if (!buf) {
            request->send(500, "application/json", "{\"ok\":0,\"error\":\"out of memory\"}");
            return;
        }
        uint16_t n = 0;
        bool tooBig = false;
        for (const TargetFilter& f : expanded) {
            uint8_t idLen = f.identifier.length();
            uint8_t descLen = min((int)f.description.length(), 48);
            if (idLen != 6 && idLen != 12) continue;
            if (n + 4 + idLen + descLen > 4096) { tooBig = true; break; }
            buf[n++] = (uint8_t)f.type;
            buf[n++] = f.isFullMAC ? 1 : 0;
            buf[n++] = idLen;
            buf[n++] = descLen;
            memcpy(buf + n, f.identifier.c_str(), idLen);     n += idLen;
            memcpy(buf + n, f.description.c_str(), descLen);  n += descLen;
        }
        uint16_t cmdId = meshNewCmdId();
        bool ok = !tooBig && meshQueueCommand(MESH_CMD_FILTERS, cmdId, buf, n);
        free(buf);
        if (!ok) {
            request->send(500, "application/json", "{\"ok\":0,\"error\":\"too many filters or mesh TX failed\"}");
            return;
        }
        request->send(200, "application/json",
                      "{\"ok\":1,\"cmd\":" + String(cmdId) + ",\"count\":" + String(expanded.size()) + "}");
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
        if (parseFiltersFromJSON(request) < 0) {
            // Parse failed before anything was erased — abort the lock so
            // the burn-in can't proceed on a half-saved configuration.
            request->send(400, "application/json",
                "{\"success\":false,\"error\":\"filter list parse error — configuration unchanged\"}");
            return;
        }

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

        // Burn-in applies the full form, exactly like /save — a burned-in
        // home node must wake up with its MQTT uplink and mesh role intact,
        // or the fleet loses its gateway on the first locked boot.
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

        meshCfg.enabled = request->hasParam("mesh_en", true);
        if (request->hasParam("mesh_role", true)) {
            meshCfg.home = request->getParam("mesh_role", true)->value() == "home";
        }
        if (request->hasParam("mesh_ch", true)) {
            int ch = request->getParam("mesh_ch", true)->value().toInt();
            if (ch >= 1 && ch <= 11) meshCfg.channel = (uint8_t)ch;
        }
        if (request->hasParam("mesh_key_clear", true)) {
            meshSetKey("");
        } else if (request->hasParam("mesh_key", true)) {
            String k = request->getParam("mesh_key", true)->value();
            k.trim();
            if (k.length() >= 8 && k.length() <= 63) meshSetKey(k.c_str());
        }
        mesh_saveConfig();

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
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { 
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
            margin: 0; 
            padding: 20px;
            background: linear-gradient(135deg, #1a1a1a 0%%, #0a0a0a 100%%); 
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
            background: linear-gradient(135deg, #2d2d2d 0%%, #1a1a1a 100%%);
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
            background: linear-gradient(135deg, #1a0a0a 0%%, #2d0a0a 100%%);
            color: #ffcccc; 
            border: 2px solid #8b0000; 
            padding: 25px; 
            border-radius: 10px; 
            margin: 25px 0; 
            font-weight: 500;
            box-shadow: 0 4px 15px rgba(139, 0, 0, 0.3);
        }
        .info {
            background: linear-gradient(135deg, #0a1a0a 0%%, #0a2d0a 100%%);
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
    // POST body/query: name=axon | meta | ipro | verkada
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
        } else if (presetName == "verkada") {
            label = "Verkada";
            added = applyPreset(PRESET_VERKADA, PRESET_VERKADA_COUNT, "Verkada");
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
        else if (n == "verkada")   removed = removePreset(PRESET_VERKADA, PRESET_VERKADA_COUNT);
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
        body += ",\"verkada\":";
        body += presetInstalled(PRESET_VERKADA, PRESET_VERKADA_COUNT) ? "true" : "false";
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

    // Catch-all: every OTHER captive-portal probe (OEM endpoints, Ubuntu's
    // connectivity-check, carrier variants, favicon requests on random
    // intercepted hosts) gets pushed to the portal too. Without this,
    // devices whose probe path isn't listed above show "no internet" and
    // never pop the portal sheet. GET/HEAD redirect; anything else 404s.
    server.onNotFound([](AsyncWebServerRequest *request) {
        if (request->method() == HTTP_GET || request->method() == HTTP_HEAD) {
            request->redirect("http://192.168.4.1/");
        } else {
            request->send(404);
        }
    });

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
    bool hasWifiFilters, hasBleFilters;
    scanDomains(hasWifiFilters, hasBleFilters);

    if (isSerialConnected()) {
        Serial.println("\n=== STARTING SCANNING MODE ===");
        Serial.println("Configured Filters:");
        for (const TargetFilter& filter : targetFilters) {
            String type = String(filterTypeCode(filter.type));
            Serial.println("- " + filter.identifier + " (" + type + "): " + filter.description);
        }
        Serial.println("==============================\n");
    }

    if (meshCfg.enabled) {
        if (isSerialConnected()) {
            Serial.printf("Node Mesh: %s node on channel %d — detections flood mesh-wide\n",
                          meshCfg.home ? "HOME (MQTT uplink)" : "REMOTE (mesh uplink)",
                          meshCfg.channel);
        }
    }

    // Ready jingle plays LAST, after scan_init() has brought the radio fully
    // up: the moment the final power-up note ends, the first sweep starts.
    // The fanfare already played once at power-on — it does not repeat here.
    delay(200);
    ascendingBeeps();

    // Transition to the appropriate initial phase
    g_phaseStartTime = millis();
    g_lastHopTime = millis();
    if (hasWifiFilters) {
        scan_to_wifi_sweep();
    } else if (hasBleFilters) {
        scan_to_ble_scan();
    } else if (meshCfg.enabled) {
        // Mesh-only (no filters yet): sit on the mesh channel so the node
        // joins, answers the dashboard and takes its filter list live.
        g_startedEmpty = true;
        meshParkChannel();
        if (isSerialConnected()) {
            Serial.println("[MESH] no filters yet: mesh-only, waiting for a filter list from the dashboard");
        }
    }
}



// ================================
// Setup Function
// ================================
void setup() {
    // First sound within ~50 ms of power-on: the fanfare plays while the
    // rest of the board wakes up. The Mario "ready" pair still plays at the
    // END of startScanningMode — after all checks — as the truthful signal.
    initializeBuzzer();
    playBoot();

    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);   // runtime BOOT-hold escape
    delay(400);
    
    // Initialize Serial first. 4 KB RX: firmware staging lines (fwdata)
    // are ~1 KB and the default 256-byte buffer drops their tail.
    Serial.setRxBufferSize(4096);
    // TX too: the 256-byte default made every long line (TLM, RST) wait on
    // the host, stalling the loop for seconds during a firmware upload.
    Serial.setTxBufferSize(4096);
    Serial.begin(115200);
    delay(250);

    // Never let the WiFi driver persist radio settings to flash: the mesh's
    // LR protocol bit was surviving reboots there and leaking onto the AP.
    WiFi.persistent(false);

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

    if (isSerialConnected()) Serial.println("[BOOT] Radio bring-up starting");

    initializeNeoPixel();
    
    // Test NeoPixel
    setNeoPixelColor(255, 0, 255); // Bright pink
    delay(350);
    setNeoPixelColor(128, 0, 255); // Purple
    delay(350);
    
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
        mesh_loadConfig();
        loadDeviceAliases();
    }

    // Detections live in LittleFS as a rolling session, promoted to
    // /session_prev.json on each boot. devices[] starts empty; the
    // dashboard's PREVIOUS SESSION panel reads the prev file directly.
    rotateSessionFiles();
    
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
    } else if (wdOn()) {
        // A wardriver needs no setup: it scans as soon as it is plugged in. (Its mesh
        // settings and fleet key, when given over serial, still apply.)
        Serial.println("WARDRIVER: scanning everything (no filters, no setup)");
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
// Machine-readable telemetry for the home dashboard (home-dashboard/):
// one "TLM {json}" line every 5 s with this node's identity, mesh counters
// and the live roster (meshNodesJson). Human logs stay as they are; the
// dashboard parses only lines starting with "TLM " (and the detection JSON).
static String jsonEsc(const String& in) {
    String out;
    out.reserve(in.length() + 4);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in.charAt(i);
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((uint8_t)c < 0x20) out += ' ';
        else out += c;
    }
    return out;
}

// Free NVS entries (32 bytes each): small settings only live there now; the
// dashboard warns when it runs low, before writes start failing silently.
static unsigned nvsFreeEntries() {
    nvs_stats_t st;
    return nvs_get_stats(NULL, &st) == ESP_OK ? (unsigned)st.free_entries : 0;
}

static void emitTelemetry() {
    if (!isSerialConnected()) return;
    uint32_t rxAll=0, rxQ=0, rxDup=0, rxSelf=0, rxBad=0, txS=0, txF=0, txC=0;
    if (meshCfg.enabled) meshStats(rxAll, rxQ, rxDup, rxSelf, rxBad, txS, txF, txC);
    uint32_t dSent=0, dRetry=0, dAck=0, dLost=0; uint8_t dPend=0;
    if (meshCfg.enabled) meshDeliveryStats(dSent, dRetry, dAck, dLost, dPend);
    uint8_t park = 0, parkBle = 0;
    if (meshCfg.enabled) meshDutyStats(park, parkBle);
    String stray = "[]";
    if (meshCfg.enabled) meshStraysJson(stray);
    bool encOn = false;
    uint32_t eDropPlain = 0, eDropAuth = 0, eDropReplay = 0;
    if (meshCfg.enabled) meshEncStats(encOn, eDropPlain, eDropAuth, eDropReplay);
    char selfId[7];
    meshSelfIdHex(selfId);
    String roster = "{\"nodes\":[]}";
    if (meshCfg.enabled) meshNodesJson(roster);
    // meshNodesJson emits {"nodes":[...]}; splice its body into ours.
    String nodes = roster.substring(1, roster.length() - 1);
    char wdch[40] = "";
    if (wdOn()) wdChannelsText(wdch, sizeof wdch);
    Serial.printf("TLM {\"v\":1,\"id\":\"%s\",\"role\":\"%s\",\"mode\":\"%s\",\"mesh\":%d,"
                  "\"ch\":%d,\"up\":%lu,\"heap\":%lu,\"dev\":%u,\"filters\":%u,\"n\":%d,"
                  "\"rx\":%lu,\"q\":%lu,\"dup\":%lu,\"self\":%lu,\"bad\":%lu,"
                  "\"tx\":%lu,\"txf\":%lu,\"txcb\":%lu,\"pend\":%u,\"lrssi\":%d,"
                  "\"heal\":%d,\"mqtt\":%d,\"dsent\":%lu,\"dretry\":%lu,\"dack\":%lu,"
                  "\"dlost\":%lu,\"dpend\":%u,\"park\":%u,\"pble\":%u,\"rate\":\"%s\",\"enc\":%d,\"eplain\":%lu,\"eauth\":%lu,\"ereplay\":%lu,\"nvs\":%u,\"kind\":\"%s\",\"rid\":%d,\"drones\":%u,\"wd\":[%lu,%lu],\"wdch\":\"%s\",\"wdon\":%d,\"strays\":%s,%s}\n",
                  selfId, meshCfg.enabled ? (meshCfg.home ? "home" : "remote") : "solo",
                  currentMode == SCANNING_MODE ? "scan" : "config",
                  meshCfg.enabled ? 1 : 0, meshCfg.channel,
                  (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap(),
                  (unsigned)devices.size(), (unsigned)targetFilters.size(),
                  meshCfg.enabled ? meshNodeCount() : 0,
                  (unsigned long)rxAll, (unsigned long)rxQ, (unsigned long)rxDup,
                  (unsigned long)rxSelf, (unsigned long)rxBad,
                  (unsigned long)txS, (unsigned long)txF, (unsigned long)txC,
                  meshCfg.enabled ? meshTxPending() : 0,
                  meshCfg.enabled ? (int)meshLinkRssi() : 0,
                  (meshCfg.enabled && meshHealing()) ? 1 : 0,
                  mqttConnected ? 1 : 0, (unsigned long)dSent, (unsigned long)dRetry,
                  (unsigned long)dAck, (unsigned long)dLost, (unsigned)dPend,
                  (unsigned)park, (unsigned)parkBle, meshCfg.enabled ? meshTxRateName() : "off",
                  encOn ? 1 : 0, (unsigned long)eDropPlain, (unsigned long)eDropAuth,
                  (unsigned long)eDropReplay, nvsFreeEntries(), FW_KIND + 12, ridEnabled ? 1 : 0, (unsigned)ridActive(),
                  (unsigned long)wdWifiCount(), (unsigned long)wdBleCount(), wdch, wdActive() ? 1 : 0, stray.c_str(), nodes.c_str());
}

void loop() {
    checkBootButtonLoop();   // BOOT hold -> clear lock, back to config mode
    meshOtaTick();
    otaRollbackTick();
    // Once this image is confirmed, drop the old-layout filter keys that were
    // kept as the rollback's copy (the blob is the list from now on).
    static bool legacySwept = false;
    if (!legacySwept && !imageOnProbation()) {
        legacySwept = true;
        preferences.begin("ouispy", false);
        int legacy = preferences.getInt("filterCount", -1);
        bool haveFile = filterFsReady() && LittleFS.exists(FILTER_FILE);
        if (legacy >= 0 && (haveFile || preferences.isKey(FILTER_BLOB_KEY))) {
            eraseLegacyFilterKeys(max(legacy, 100));
            preferences.remove("filterCount");
            if (isSerialConnected()) Serial.println("[NVS] old filter keys removed (firmware confirmed)");
        }
        if (haveFile && preferences.isKey(FILTER_BLOB_KEY)) {
            preferences.remove(FILTER_BLOB_KEY);   // the rollback copy, no longer needed
            if (isSerialConnected()) Serial.println("[NVS] filter blob moved to the file system (firmware confirmed)");
        }
        preferences.end();
    }
    static unsigned long lastCleanupTime = 0;
    static unsigned long lastStatusTime = 0;
    unsigned long currentMillis = millis();
    
    if (currentMode == CONFIG_MODE) {
        // Check for scheduled normal restart (from burn-in config)
        if (normalRestartScheduled > 0 && currentMillis >= normalRestartScheduled) {
            if (isSerialConnected()) {
                Serial.println("Scheduled normal restart - rebooting with locked configuration...");
            }

            mesh_deinit();   // goodbye the mesh if it's up (home portal mode)
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

            mesh_deinit();   // goodbye the mesh if it's up
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

        // Headless provisioning: one-line serial commands (mesh role/channel,
        // filter install, save+go) while the portal is open.
        serialConsoleTick();

        // Service the mesh while the portal is open: command/ack traffic for
        // the management panel, presence announcements, relays. Detections
        // received here pile into the (undrained) ring and drop at full —
        // the home isn't aggregating while it's being configured.
        if (meshCfg.enabled) {
            meshTick();
            remoteMgmtTick();
            meshLinkLedTick();
            static unsigned long lastMeshLinkLog = 0;
            if (millis() - lastMeshLinkLog >= 10000) {
                lastMeshLinkLog = millis();
                if (isSerialConnected()) {
                    Serial.printf("[MESH] link: %d node(s) in roster\n", meshNodeCount());
                }
            }
            static unsigned long lastTlm = 0;
            if (millis() - lastTlm >= 5000) {
                lastTlm = millis();
                emitTelemetry();
            }
        }

        // Handle web server
        delay(100);
        return;
    }
    
    // Scanning mode loop — time-sliced WiFi/BLE with ring-buffer drain.
    // Single pass per loop() invocation: the top-of-loop housekeeping
    // (checkBootButtonLoop) and the bottom NeoPixel animation must get a
    // chance to run every iteration.
    if (currentMode == SCANNING_MODE) {
        unsigned long currentMillis = millis();
        bool hasWifiFilters, hasBleFilters;
        scanDomains(hasWifiFilters, hasBleFilters);

        if (!hasWifiFilters && !hasBleFilters) {
            // No detection work. With the mesh enabled this node is a pure
            // relay — keep the radio parked on the mesh channel and service
            // the mesh instead of powering the radio down.
            if (meshCfg.enabled) {
                // Frames only go out while parked; nothing else moves the
                // channel on a mesh-only node, so park once.
                static bool reliefParked = false;
                if (!reliefParked) { meshParkChannel(); reliefParked = true; }
                meshTick();
                remoteMgmtTick();
                serialConsoleTick();
            } else {
                scan_exit();
                delay(100);
            }
            return;
        }

        // WiFi-only scanning never reaches a BLE phase, so the time-sliced
        // MQTT report window can never open — say so once instead of
        // silently dropping queued events. (With the mesh enabled, WiFi-only
        // configs DO alternate: the BLE phase is the mesh window.)
        static bool wifiOnlyMqttNoted = false;
        if (!wifiOnlyMqttNoted && hasWifiFilters && !hasBleFilters && mqttCfg.enabled && !meshCfg.enabled) {
            wifiOnlyMqttNoted = true;
            if (isSerialConnected()) {
                Serial.println("[MQTT] WiFi-only scanning keeps STA down; detections are not reported (see README)");
            }
        }

        // Channel auto-heal owns the radio while sweeping: no phase
        // transitions, no hops — just the mesh, the console, and the LED.
        // Firmware update in progress (sending or receiving): scanning
        // pauses and the radio stays parked on the mesh channel.
        static bool otaParked = false;
        if (meshCfg.enabled && meshOtaBusy()) {
            if (!otaParked) {
                if (pBLEScan && pBLEScan->isScanning()) pBLEScan->stop();
                esp_wifi_set_promiscuous(false);
                g_radioPhase = PHASE_IDLE;   // scanning restarts cleanly afterwards
                meshParkChannel();
                otaParked = true;
            }
            meshTick();
            remoteMgmtTick();
            serialConsoleTick();
            updateNeoPixelAnimation();
            meshLinkLedTick();
            // Keep telemetry flowing, or the dashboard shows the fleet offline.
            static unsigned long otaTlmAt = 0;
            if (millis() - otaTlmAt >= 5000) { otaTlmAt = millis(); emitTelemetry(); }
            delay(1);
            return;
        }
        otaParked = false;

        if (meshCfg.enabled && meshHealing()) {
            meshTick();
            remoteMgmtTick();
            serialConsoleTick();
            updateNeoPixelAnimation();
            meshLinkLedTick();
            delay(1);
            return;
        }

        // Time-sliced MQTT report window: only while promiscuous is off and
        // the BLE scan window has expired. A running report cycle freezes
        // the phase transitions below so the radio stays off promiscuous
        // until the flush completes or its deadline passes.
        mqttReportTick(currentMillis, g_mqttRadioTimeslice &&
                                      g_radioPhase == PHASE_BLE_SCAN &&
                                      pBLEScan && !pBLEScan->isScanning());

        unsigned long phaseDur = (g_radioPhase == PHASE_WIFI_SWEEP) ? WIFI_SWEEP_MS : BLE_SCAN_MS;

        // Mesh priority: while the mesh has something time-critical (a
        // command going out, replies expected, a fresh detection) the radio
        // stays on the mesh channel WITHOUT a BLE scan. Coexistence gives a
        // running BLE scan ~2/3 of the airtime, and that is exactly where
        // replies were being lost. Capped at 10 s of continuous urgency so a
        // stuck flag can never starve detection scanning.
        static unsigned long urgentSince = 0;
        bool meshPriority = false;
        if (meshCfg.enabled && !meshHealing() && meshNeedsAir()) {
            if (!urgentSince) urgentSince = currentMillis ? currentMillis : 1;
            meshPriority = currentMillis - urgentSince < 10000;
        } else {
            urgentSince = 0;
        }
        if (meshPriority && g_mqttReportState == MR_IDLE && g_radioPhase == PHASE_BLE_SCAN) {
            if (hasWifiFilters) {
                scan_to_wifi_sweep();            // the sweep then parks on the mesh (below)
                g_phaseStartTime = currentMillis;
            } else if (pBLEScan && pBLEScan->isScanning()) {
                pBLEScan->stop();                // BLE-only: already parked, just pause BLE
            }
        }

        // Phase transition on timeout (frozen while an MQTT report cycle
        // owns the radio, and no BLE phase while the mesh has priority).
        if (g_mqttReportState == MR_IDLE && currentMillis - g_phaseStartTime >= phaseDur) {
            // Alternate when both domains have filters — or when the mesh is
            // enabled and WiFi filters exist: the BLE phase doubles as the
            // mesh window, so WiFi-only nodes must still open it.
            if ((hasWifiFilters && hasBleFilters) || (meshCfg.enabled && hasWifiFilters)) {
                if (g_radioPhase == PHASE_WIFI_SWEEP) {
                    if (!meshPriority) scan_to_ble_scan();
                } else {
                    scan_to_wifi_sweep();
                }
            }
            g_phaseStartTime = currentMillis;
        }

        // Single-domain BLE: the 2 s NimBLE scan window expires before the
        // 2.2 s phase timer, and with no WiFi domain there's no alternating
        // transition to re-arm it — top it up here so BLE-only configs keep
        // scanning instead of going silent after the first window.
        if (g_mqttReportState == MR_IDLE && hasBleFilters && !hasWifiFilters && !meshPriority &&
            g_radioPhase == PHASE_BLE_SCAN && pBLEScan && !pBLEScan->isScanning()) {
            pBLEScan->start(2, nullptr, false);
            g_phaseStartTime = currentMillis;
        }

        // Channel hop while in WiFi sweep phase. The mesh channel gets a
        // longer dwell — AND the mesh parks there during it, so queued
        // mesh frames flush mid-sweep instead of waiting for the next BLE
        // window. Without this the peer receives in clumps spaced one full
        // phase cycle apart, and one late clump blinks the link light.
        if (g_radioPhase == PHASE_WIFI_SWEEP && hasWifiFilters) {
            unsigned long dwell = g_meshVisit ? MESH_VISIT_MS
                                : (g_wifiChannel == meshCfg.channel) ? MESH_CH_DWELL_MS : WIFI_HOP_MS;
            bool urgent = meshCfg.enabled && !meshHealing() && meshUrgent();
            if (urgent && !g_meshVisit && g_wifiChannel != meshCfg.channel) {
                meshParkChannel();          // something to deliver: go to the mesh now
                g_meshVisit = true;
                g_lastHopTime = currentMillis;
            } else if (urgent && g_meshVisit) {
                g_lastHopTime = currentMillis;   // hold the mesh channel until it clears
            } else if (currentMillis - g_lastHopTime >= dwell) {
                if (meshCfg.enabled && !meshHealing() && !g_meshVisit &&
                    g_wifiChannel != meshCfg.channel) {
                    meshParkChannel();      // short mesh visit between foreign channels
                    g_meshVisit = true;
                } else {
                    g_meshVisit = false;
                    scan_wifi_hop_channel();
                    if (g_wifiChannel == meshCfg.channel) meshParkChannel();
                    else meshUnparkChannel();
                }
                g_lastHopTime = currentMillis;
            }
        }

        ridLoop();
        if (wdOn()) wdLoop();
#ifdef SKYSPY_NODE
        skyspyRadioTick();
#endif

        // Drain one ring entry per pass. Dedup, cooldown, device tracking,
        // alerts, MQTT and the session file all happen here in loop context
        // — never in the NimBLE/WiFi callbacks (see registerDetection).
        if (detQueueCount > 0) {
            DetectionEntry e;
            portENTER_CRITICAL(&detQueueMux);
            e = detQueue[detQueueTail];
            detQueueTail = (detQueueTail + 1) % DET_QUEUE_SIZE;
            detQueueCount--;
            portEXIT_CRITICAL(&detQueueMux);

            String mac = e.mac;
            String ident = e.identifier;
            String desc = e.description;
            // node = which mesh node saw it (stable ID): mesh-relayed
            // detections carry a "[mesh XXXXXX]" desc suffix (mesh.cpp),
            // everything else is our own radio.
            int tag = desc.lastIndexOf("[mesh ");
            bool fromMesh = (tag >= 0);
            // Our own radio: the cooldown ladder decides alerts and reports.
            // Over the mesh: the origin node already rate-limited it, so it
            // is always reported (the home hearing the same device itself
            // used to swallow every remote's report of it); the ladder here
            // only decides whether THIS node beeps. Signal updates never do.
            const char* label = e.update ? nullptr
                                         : registerDetection(e.mac, e.matchedType, e.rssi,
                                                             e.wifiDomain, ident, desc);
            bool update = e.update || (label && strcmp(label, "UPD") == 0);
            if (label != nullptr || fromMesh) {
                String alias = getDeviceAlias(mac);
                // dom = BLE vs WiFi.
                char nodeId[7];
                if (tag >= 0 && desc.length() >= (unsigned)tag + 13 && desc.charAt(tag + 12) == ']') {
                    strlcpy(nodeId, desc.c_str() + tag + 6, sizeof(nodeId));
                } else {
                    meshSelfIdHex(nodeId);
                }
                String payload = "{\"mac\":\"" + mac + "\",\"alias\":\"" + jsonEsc(alias) +
                                 "\",\"rssi\":" + String(e.rssi) + ",\"type\":\"" +
                                 filterTypeCode(e.matchedType) + "\",\"match\":\"" +
                                 jsonEsc(ident) + "\",\"desc\":\"" + jsonEsc(desc) +
                                 "\",\"node\":\"" + nodeId + "\",\"dom\":\"" +
                                 (e.wifiDomain ? "wifi" : "ble") + "\"" +
                                 (update ? ",\"upd\":1" : "");
                String infoJson = devinfoJson(e.info, e.infoLen);
                if (infoJson.length()) payload += ",\"info\":" + infoJson;
                payload += "}";
                if (isSerialConnected()) Serial.println(payload);

                // Remote-role nodes never publish MQTT — the mesh is their
                // uplink. Home nodes (and unmeshed nodes) publish as before.
                if (mqttCfg.enabled && !meshIsRemoteUplink()) {
                    if (!hasWifiFilters) {
                        // STA stays up for good in BLE-only configs; publish
                        // directly (mqtt_loop supervises the connection).
                        if (mqttConnected) mqtt_publish(mqttCfg.topic, payload.c_str());
                    } else {
                        // Radio is time-sliced; flushed during a BLE phase by
                        // mqttReportTick().
                        if (!mqttPendingPush(payload) && isSerialConnected()) {
                            Serial.println("[MQTT] report queue full, event dropped");
                        }
                    }
                    lastDetectionTime = currentMillis;
                    detectionActive = true;
                }

                // Send our OWN radio's detections mesh-wide (retried until a
                // home acks). Detections that arrived over the mesh already
                // flooded from their origin; re-sending them would credit
                // them to this node and multiply traffic.
                if (meshCfg.enabled && !fromMesh) {
                    meshQueueDetection(e.mac, e.identifier, e.description,
                                       e.rssi, (uint8_t)e.matchedType, e.wifiDomain, update,
                                       e.info, e.infoLen);
                }

                // Alert the user. Doing it here — not in the NimBLE host
                // callback or the WiFi driver task — keeps multi-hundred-ms
                // beeps from destabilizing those stacks.
                if (label && !update) {
                    startDetectionFlash();
                    if (strcmp(label, "RE-3s") == 0) {
                        twoBeeps();
                    } else {
                        threeBeeps();   // "NEW" and "RE-30s"
                    }
                }
            }
        }

        // Flush the rolling LittleFS session whenever a NEW detection
        // landed. Doing the I/O here (not in a callback) keeps NimBLE off
        // the flash driver's toes.
        if (sessionDirty) {
            sessionDirty = false;
            writeCurrentSession();
        }

        // Belt-and-suspenders periodic flush in case last_ms on existing
        // rows drifted and we want it durable across a crash.
        if (currentMillis - lastCleanupTime >= 30000) {
            lastCleanupTime = currentMillis;
            if (!devices.empty()) writeCurrentSession();
        }

        // Status report disabled - using JSON output only
        if (currentMillis - lastStatusTime >= 30000) {
            lastStatusTime = currentMillis;
        }

        mqtt_loop(currentMillis);

        // Node mesh housekeeping: inject received detections into the ring
        // above (they alert + publish like local ones), relay floods, flush
        // the TX queue, announce presence, prune the roster.
        if (meshCfg.enabled) meshTick();
        remoteMgmtTick();

        // Locked-node management channel: accepts ONLY 'meshrechan <ch>' —
        // see serialConsoleTick. Everything else is config-mode-only.
        serialConsoleTick();

        // Periodic heartbeat to confirm loop is alive
        static unsigned long lastHeartbeat = 0;
        if (currentMillis - lastHeartbeat >= 5000) {
            lastHeartbeat = currentMillis;
            uint32_t rxAll=0, rxQ=0, rxDup=0, rxSelf=0, rxBad=0, txS=0, txF=0, txC=0;
            if (meshCfg.enabled) meshStats(rxAll, rxQ, rxDup, rxSelf, rxBad, txS, txF, txC);
            if (isSerialConnected()) Serial.printf("[HEARTBEAT] phase=%d ch=%d dev=%zu mesh=%d rx=%lu q=%lu dup=%lu self=%lu bad=%lu tx=%lu/%lu/%lu led=%d flag=%d idle=%lu lrssi=%d relay=%lu/%lu\n",
                g_radioPhase, g_wifiChannel, devices.size(),
                meshCfg.enabled ? meshNodeCount() : 0,
                (unsigned long)rxAll, (unsigned long)rxQ, (unsigned long)rxDup,
                (unsigned long)rxSelf, (unsigned long)rxBad,
                (unsigned long)txS, (unsigned long)txF, (unsigned long)txC,
                digitalRead(LED_PIN), ledEnabled ? 1 : 0,
                (unsigned long)(millis() - lastLedActivity),
                meshCfg.enabled ? (int)meshLinkRssi() : 0,
                (unsigned long)meshRelaySent, (unsigned long)meshRelaySuppressed);
            emitTelemetry();
        }

        updateNeoPixelAnimation();
        meshLinkLedTick();
        delay(1);
        return;
    }

    // Update NeoPixel animation
    updateNeoPixelAnimation();
    
    delay(100);
} 