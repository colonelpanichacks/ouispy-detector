#pragma once
#include <Arduino.h>
#include <stdint.h>

// ================================
// Node Mesh (ESP-NOW Long Range)
// ================================
// Every node runs the same firmware; a config flag decides the role:
//   - remote node: detections are published into the mesh instead of MQTT
//   - home node:   receives mesh-wide detections and reports them via MQTT
// Packets are flooded with an (origin, seq) duplicate cache and a TTL hop
// limit, so any-to-any connectivity emerges without a peer table. ESP-NOW
// frames are vendor-specific action frames; every node enables the LR
// protocol on its STA interface. ESP-NOW itself goes out at the driver's
// default rate unless the build defines MESH_ESPNOW_LR_RATE (e.g.
// WIFI_PHY_RATE_LORA_250K) — a fleet-wide setting, all nodes alike.

struct MeshConfig {
    bool enabled;
    bool home;        // true = home node (MQTT uplink), false = remote node
    uint8_t channel;  // 1-11, must match on every node
    char key[64];     // fleet key: signs commands (empty = unauthenticated)
};

extern MeshConfig meshCfg;

void mesh_loadConfig();
void mesh_saveConfig();

// Fleet key. When set, every command this node sends is signed
// (HMAC-SHA256, with a persisted counter against replays), and it only
// obeys commands signed with the same key. Setting a new key forgets all
// remembered counters. Persists to NVS.
bool meshKeySet();
void meshSetKey(const char* key);
// Lost nodes: their packets are dropped (not relayed) and reported as "DENIED {...}".
void meshSetDeny(const uint8_t* ids, uint8_t n);
// A GPS node's position, flooded to the home (printed there as POS {...}).
void meshSendPos(double lat, double lon, float acc, uint8_t sats, float spd, float crs);
uint8_t meshDenyCount();
uint32_t meshDeniedRx();

// Starts ESP-NOW on the mesh channel with the LR protocol enabled. Requires
// WiFi STA mode to be started already. Returns false on init failure.
bool mesh_init();
void mesh_deinit();

// True when the mesh is enabled and this node relays to peers instead of
// publishing MQTT itself (remote role).
bool meshIsRemoteUplink();

// Queue a LOCAL detection for mesh-wide delivery (never re-queue one that
// arrived over the mesh). Retried until a home acks it. loop() only.
// update = a signal update for a device still in view (flags bit1): no alert.
void meshQueueDetection(const char* mac, const char* ident, const char* desc,
                        int rssi, uint8_t dtype, bool wifiDomain, bool update = false,
                        const uint8_t* info = nullptr, uint8_t infoLen = 0);

// Called when the radio is parked on the mesh channel (mesh RX possible).
void meshParkChannel();
// Called when something else owns the radio (WiFi sweep / MQTT STA window).
void meshUnparkChannel();
// Fleet-key encryption: whether it's on, and frames dropped as plaintext
// (not our fleet), failing authentication (wrong key / tampered) or replayed.
void meshEncStats(bool& on, uint32_t& dropPlain, uint32_t& dropAuth, uint32_t& dropReplay);
// ESP-NOW TX rate actually in force: "lr250k", "lr500k", "1m" (LR not
// built in) or "lr-failed" (the driver refused the LR rate).
const char* meshTxRateName();
// True while the mesh has work that justifies pausing BLE scanning: a
// command transfer, a listen window (replies owed/expected), a fresh
// detection. Unlike meshUrgent(), queued routine announces don't count.
bool meshNeedsAir();
// Keep the radio on the mesh channel (meshUrgent() true) for at least ms:
// used while commands go out and while this node owes a reply.
void meshHoldRadio(uint32_t ms);
// The main loop's WiFi sweep moved the radio to channel ch (any channel).
void meshNoteRadioChannel(uint8_t ch);
// Nodes heard on another channel than ours in the last 60 s, as a JSON
// array: [{"id","ch","last"}]. A healthy fleet has none.
void meshStraysJson(String& out);
// Tell the duty accounting whether a BLE scan shares the radio right now.
void meshSetBlePhase(bool scanning);
// Rolling 10 s: % of time parked on the mesh channel, and % parked while a
// BLE scan was also running (coexistence takes most of that airtime).
void meshDutyStats(uint8_t& parkPct, uint8_t& parkBlePct);

// loop()-context housekeeping: drain received packets (inject + relay),
// flush the TX queue, send periodic presence announcements, prune the node
// roster. Never call from an interrupt or driver callback.
void meshTick();

// Reliable detection delivery counters: first sends, retries, detections
// acked by a home, detections given up (no ack in 30 s / table overflow),
// and how many are still in flight.
void meshDeliveryStats(uint32_t& sent, uint32_t& retries, uint32_t& acked,
                       uint32_t& expired, uint8_t& pending);

// True while a fresh detection, queued frame or command transfer is waiting
// to go out: the WiFi sweep holds the mesh channel until it clears.
bool meshUrgent();

// Number of distinct mesh nodes heard from in the roster window.
uint8_t meshNodeCount();

// Link diagnostics for the heartbeat line: frames seen by the ESP-NOW
// callback vs queued/dropped, plus TX attempts and failures.
void meshStats(uint32_t& rxAll, uint32_t& rxQueued, uint32_t& rxDup,
               uint32_t& rxSelf, uint32_t& rxBad,
               uint32_t& txSent, uint32_t& txFail, uint32_t& txCbFail);

// Managed-flood relays: frames this node re-broadcast vs ones it held back
// because other nodes had already carried them.
extern uint32_t meshRelaySent, meshRelaySuppressed;

// Called from the promiscuous callback when an air frame carries our mesh
// magic — records RSSI for range testing (driver context, keep it cheap).
void meshSniffRssi(const uint8_t* mac, int8_t rssi);
// Freshest sniffed mesh RSSI within the last 10 s, 0 = silence.
int8_t meshLinkRssi();
// Link-LED truth: any frame from any node within the tight freshness
// window (~4.5 s). Stricter than the roster timeout so a dead peer's light
// goes dark quickly while management state (roster) rides the longer one.
bool meshLinkFresh();
// True while channel auto-heal is sweeping — the main loop suppresses the
// scan phase machine so nothing fights the per-channel dwell.
bool meshHealing();
// Allow/forbid channel auto-heal. The config-mode portal AP shares the radio
// in AP_STA, so a heal sweep would drag the AP across channels: the home's
// portal turns it off.
void meshSetHealAllowed(bool allowed);

// ================================
// Command channel (protocol v2)
// ================================
// The home node manages remotes over the same flood: commands are broadcast
// packets every node applies and relays; a staged-chunk protocol carries
// payloads larger than one ESP-NOW frame (filter lists), verified with a
// CRC32 before applying. Nodes answer with flooded acks so the home can
// count how many nodes applied a command, including ones it can't reach
// directly. With a fleet key set, commands are signed and verified.
//
// Commands (home -> all nodes):
#define MESH_CMD_AUDIO   1   // payload: u8 buzzerOn, u8 ledOn (live, persisted)
#define MESH_CMD_FILTERS 2   // payload: binary TargetFilter records; replaces the list (live)
#define MESH_CMD_REBOOT  3   // payload: none
#define MESH_CMD_SETCHAN 4   // payload: u8 channel 1-11 (NVS + reboot to apply)
// Remote management (v3 extension; older firmware acks these as rejected):
#define MESH_CMD_TARGET  5   // payload: u8 target[3], u8 innerCmd, inner payload.
                             // Only the node whose stable ID matches (000000 =
                             // every node) applies the inner command; others
                             // stay silent. SETCHAN is fleet-wide only.
#define MESH_CMD_STATUS  6   // payload: none. Node floods a STATUS packet (jittered).
#define MESH_CMD_IDENTIFY 7  // payload: u8 seconds, u8 flags (bit0 beep, bit1 light)
#define MESH_CMD_PRESET  8   // payload: u8 preset (0 axon, 1 meta, 2 ipro, 3 verkada); live
#define MESH_CMD_GETFILTERS 9      // payload: none. Node answers with FILTERS_REPORT.
#define MESH_CMD_FILTERS_REPORT 10 // payload: filter records (node -> home). Never
                                   // applied or acked; the home prints it.
#define MESH_CMD_FILTER_ADD 11     // payload: filter records merged into the list (live)
#define MESH_CMD_FILTER_DEL 12     // payload: filter records removed by (type, id) (live)
#define MESH_CMD_ALIAS 14          // payload: the node's new alias (0-32 plain ASCII, empty clears)
#define MESH_CMD_WIPE 17           // payload: "WIPE". Erase keys, settings, filters, logs and the firmware: unusable
#define MESH_CMD_DENY 18           // payload: n x 3-byte node ids (n may be 0): the lost-node list, replaces it
#define MESH_CMD_LOCATE 16         // payload: u8 mode: 0 stop, 1 one triple beep, 2 triple beeps until stopped
#define MESH_CMD_WARDRIVE 15       // payload: u8 255, u8 raw (255 keep), u16 le channel mask (0 keep); wardrive.h
// Filter edits ack with count = filters installed afterwards; status 1 when
// refused (invalid records, or the node would be left with no filters).
// Ack body (remote -> home): u8 cmd, u16 cmdId, u8 status, u8 count
// status: 0 = applied, 1 = rejected, 2 = fleet key mismatch, 3 = replayed

// Flood a STATUS packet: body = {selfId[3], data[len]} (len <= 200).
void meshSendStatus(const uint8_t* data, uint8_t len);

uint16_t meshNewCmdId();
// Floods payload in chunks, fed into the TX queue as it drains. Returns
// false when the mesh is down, another command is still going out, or the
// payload exceeds the staging buffer (4096 bytes, 4076 when signed).
bool meshQueueCommand(uint8_t cmd, uint16_t cmdId, const uint8_t* payload, uint16_t len);
// Queue an ack (flooded like detections).
void meshSendAck(uint8_t cmd, uint16_t cmdId, uint8_t status, uint8_t count);
// Number of successful acks received for cmdId (0 when unknown).
uint8_t meshCmdAckCount(uint16_t cmdId);
// Roster snapshot as JSON: {"nodes":[{id,role,last,pkts}...]}
void meshNodesJson(String& out);
// Frames still to send: TX queue plus command chunks not yet queued.
uint8_t meshTxPending();
// This node's stable ID (factory eFuse MAC tail) as 6 hex chars + NUL.
// Valid whether or not the mesh is running.
void meshSelfIdHex(char out[7]);
void meshSelfId(uint8_t out[3]);
void meshSendDrone(const uint8_t* body, uint8_t len);
// Push everything pending in the TX queue immediately (used right before a
// command-triggered reboot so the ack actually leaves the radio). No-op
// unless the radio is parked on the mesh channel — see meshFlushTxNow.
void meshFlushTxNow();

// For mesh_ota.cpp: raw OTA frames and the bits of mesh state it needs.
void meshSelfIdBytes(uint8_t out[3]);
bool meshSealed();                 // fleet key set and frames encrypted
uint8_t meshTxFree();
bool meshOtaSend(const uint8_t* body, uint8_t len, bool relay);
bool meshHomeHeard();              // the home is live in the roster
// Direct neighbors (heard first-hand in the last 20 s), 5 bytes each:
// id[3], rssi (i8, 0 = not measured yet), seconds since last heard.
uint8_t meshNeighbors(uint8_t* out, uint8_t max);

// Implemented in main.cpp — applies a complete, CRC-verified command payload
// in loop context. May reboot the node (filter push, reboot command).
void meshApplyCommand(uint8_t cmd, uint16_t cmdId, const uint8_t* payload, uint16_t len);

// Implemented in main.cpp — a STATUS packet arrived from another node.
void meshOnStatus(const uint8_t* nodeId, const uint8_t* data, uint8_t len);
// Implemented in main.cpp — a command ack arrived (any status), with the
// stable ID of the node that sent it.
void meshOnAck(const uint8_t* nodeId, uint8_t cmd, uint16_t cmdId, uint8_t status, uint8_t count);

// Implemented in main.cpp — bridge into the shared detection ring buffer so a
// mesh-originated detection gets the same alert/MQTT treatment as a local one.
void meshReport(const char* mac, const char* ident, const char* desc,
                int rssi, uint8_t dtype, bool wifi, bool update,
                const uint8_t* info = nullptr, uint8_t infoLen = 0);
