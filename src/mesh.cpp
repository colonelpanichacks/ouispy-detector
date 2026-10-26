#include "mesh.h"
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Preferences.h>
#include <rom/crc.h>
#include <string.h>
#include <esp_mac.h>
#include <mbedtls/md.h>

// Serial-guard shared with main.cpp
extern bool isSerialConnected();

// ================================
// Packet layout (wire format, v3)
// ================================
//  0  magic[2]      'O' 'S'
//  2  version       3
//  3  type          1 = announce, 2 = detection, 3 = command, 4 = ack, 5 = bye
//  4  ttl           flood hop limit (relay only if > 1)
//  5  seq           origin-side rolling counter
//  6  origin[6]     MAC of the node that first produced the packet
// 12  body          ALWAYS starts with nodeId[3] — the factory eFuse MAC
//                  tail (stable across reboots; the roster and UI key on
//                  this, never on the randomized origin MAC), then per type:
//
// Announce body:  {u8 nodeId[3], u8 role}
// Detection body: {u8 nodeId[3], i8 rssi, u8 dtype, u8 flags(bit0=wifi),
//                  char mac[18], char ident[18], char desc[40]}
// Command body:   {u8 nodeId[3], u8 cmd, u16 cmdId, u16 offset, u8 more,
//                  u32 crc32, data[]}  (crc covers the reassembled payload)
// Ack body:       {u8 nodeId[3], u8 cmd, u16 cmdId, u8 status, u8 count}
// Bye body:       {u8 nodeId[3], u8 reason}
#define MESH_MAGIC0   'O'
#define MESH_MAGIC1   'S'
#define MESH_VERSION  3
#define MESH_T_ANNOUNCE 1
#define MESH_T_DETECT   2
#define MESH_T_COMMAND  3
#define MESH_T_ACK      4
#define MESH_T_BYE      5
#define MESH_TTL        3
#define MESH_HDR_LEN    12
#define MESH_MIN_LEN    (MESH_HDR_LEN + 3)   // header + nodeId: every packet has both
#define MESH_PKT_MAX    250
#define MESH_BODY       MESH_HDR_LEN
#define MESH_DETECT_LEN (MESH_HDR_LEN + 3 + 1 + 1 + 1 + 18 + 18 + 40)
#define MESH_CMD_MIN    (MESH_HDR_LEN + 3 + 1 + 2 + 2 + 1 + 4)
#define MESH_ACK_LEN    (MESH_HDR_LEN + 3 + 1 + 2 + 1 + 1)
#define MESH_BYE_LEN    (MESH_HDR_LEN + 3 + 1)
#define MESH_CMD_CHUNK  180
#define MESH_STAGE_MAX  4096
#define MESH_STAGE_CHUNKS ((MESH_STAGE_MAX + MESH_CMD_CHUNK - 1) / MESH_CMD_CHUNK)   // 23
#define MESH_STAGE_GAP_MS 60000UL

// Signed commands (fleet key): the wire cmd byte carries MESH_CMD_SIGNED and
// the reassembled payload ends in {u32 counter, u8 tag[16]}. Nodes without
// the key (or older firmware) see an unknown command and refuse it.
#define MESH_CMD_SIGNED   0x80
#define MESH_SIG_TAG_LEN  16
#define MESH_SIG_LEN      (4 + MESH_SIG_TAG_LEN)
#define MESH_KEYCTR_SLOTS 8

#define MESH_TXQ_SIZE   8    // deep queues blast too many frames at once when
                             // a window opens and wedge the driver (fails +
                             // RX silence) — 8 caps the per-flush burst
#define MESH_TXQ_RESERVE 2   // slots a command transfer leaves free for
                             // announces, acks and relays
#define MESH_RXQ_SIZE   8
#define MESH_SEEN_SIZE  32
#define MESH_SEEN_TTL_MS    60000UL
#define MESH_ROSTER_SIZE    8
#define MESH_ROSTER_TIMEOUT 8000UL    // management view: who can be managed
#define MESH_LIGHT_TIMEOUT_MS 4500UL  // link LED: freshest last-heard window
#define MESH_ANNOUNCE_MS    1500UL
#define MESH_ANNOUNCE_LONE_MS 1500UL
#define MESH_BURST_LEN      8
#define MESH_BURST_GAP_MS   120
#define MESH_CH_MIN 1
#define MESH_CH_MAX 11

MeshConfig meshCfg = { false, false, 6, "" };

static bool meshStarted = false;
static bool meshParked = false;
static uint8_t selfMac[6] = {0};
static uint8_t selfId[3] = {0};   // factory MAC tail — the stable node ID
static uint8_t meshSeq = 0;

// (origin, seq) duplicate cache — drops re-flooded copies within the TTL
// window. Our own packets never need an entry: the receive path drops
// anything carrying our origin MAC before the cache is consulted.
struct MeshSeen { uint8_t origin[6]; uint8_t seq; uint32_t ts; bool live; };
static MeshSeen seenCache[MESH_SEEN_SIZE];

// TX queue: loop-context only (meshQueueDetection / command pump / relay /
// acks), so no locking.
static uint8_t txQ[MESH_TXQ_SIZE][MESH_PKT_MAX];
static uint8_t txLen[MESH_TXQ_SIZE];
static uint8_t txHead = 0, txTail = 0, txCount = 0;

// RX ring: filled from the ESP-NOW recv callback (WiFi driver task), drained
// in meshTick — same portMUX pattern as the detection ring in main.cpp.
static uint8_t rxQ[MESH_RXQ_SIZE][MESH_PKT_MAX];
static uint8_t rxLen[MESH_RXQ_SIZE];
static uint8_t rxHead = 0, rxTail = 0, rxCount = 0;
static portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

// Roster of other nodes, keyed by the STABLE nodeId (never the MAC).
struct MeshNode { uint8_t id[3]; uint32_t lastSeen; uint8_t role; uint32_t pkts; bool live; };
static MeshNode roster[MESH_ROSTER_SIZE];

// Ack counters for the last few command batches (successful acks only).
struct MeshAck { uint16_t cmdId; uint8_t count; };
static MeshAck acks[4];

// Command reassembly staging. Chunks always start on a 180-byte boundary,
// so completeness is tracked per chunk: a partial slot can never pass for a
// received one. The transfer stays open until every chunk up to the final
// one is in (or the gap timeout), so a final chunk that overtakes an earlier
// one on a relay path no longer kills it.
static uint8_t  stageBuf[MESH_STAGE_MAX];
static uint32_t stageChunks = 0;     // bit k = chunk k received
static uint16_t stageCmdId = 0;
static uint8_t  stageCmd = 0;
static uint8_t  stageOrigin[3] = {0};
static uint32_t stageLastChunk = 0;
static bool     stageActive = false;
static bool     stageHaveFinal = false;
static uint16_t stageTotal = 0;
static uint32_t stageCrc = 0;

// Transfers already applied, so a sender's late resend can't apply twice.
struct MeshDone { uint8_t origin[3]; uint16_t cmdId; uint32_t ts; bool live; };
static MeshDone doneCache[4];

// Outgoing command transfer: chunks are fed into the TX queue as it drains,
// so one command can carry the full 4096-byte staging size without ever
// deepening the queue.
static uint8_t  outBuf[MESH_STAGE_MAX];
static uint16_t outLen = 0, outOff = 0, outCmdId = 0;
static uint8_t  outCmd = 0;
static uint32_t outCrc = 0;
static bool     outActive = false;

// Fleet key replay protection: highest counter accepted per commander, and
// this node's own send counter. Both persist across reboots.
struct MeshKeyCtr { uint8_t id[3]; uint32_t ctr; };
static MeshKeyCtr keyCtr[MESH_KEYCTR_SLOTS];
static uint32_t keyTxCtr = 0;

// RSSI sniffing: the promiscuous callback (sweep phases) sees every frame
// with rx_ctrl, including our own ESP-NOW broadcasts — signal strength is
// recorded here keyed by nodeId for range diagnostics and the UI JSON.
#define MESH_SNIFF_SLOTS 8
static uint8_t  sniffId[MESH_SNIFF_SLOTS][3];
static int8_t   sniffRssi[MESH_SNIFF_SLOTS];
static uint32_t sniffTs[MESH_SNIFF_SLOTS];

static unsigned long lastAnnounce = 0;
static uint8_t burstRemaining = 0;
static unsigned long burstNextAt = 0;
static volatile uint32_t sendFailures = 0;

// Channel auto-heal: a node that has heard nothing for a while (e.g. its
// channel setting was corrupted by an interrupted NVS write, or the fleet
// moved) sweeps the mesh channels listening for mesh traffic, adopts the
// channel it finds the fleet on, and persists it. Without this, a channel
// mismatch is a silent permanent partition.
static bool healActive = false;
static uint8_t healCh = MESH_CH_MIN;
static unsigned long healStepAt = 0;
static unsigned long lastTrafficAt = 0;
#define MESH_HEAL_AFTER_MS   120000UL
#define MESH_HEAL_DWELL_MS   3000UL

bool meshHealing() { return healActive; }
static bool healAllowed = true;
void meshSetHealAllowed(bool allowed) {
    healAllowed = allowed;
    if (!allowed && healActive) {
        // Abort a sweep already in progress and go back to the configured
        // channel, or the portal AP would keep following the sweep.
        healActive = false;
        if (meshStarted) {
            esp_wifi_set_channel(meshCfg.channel, WIFI_SECOND_CHAN_NONE);
            meshParked = true;
        }
        lastTrafficAt = millis();
    }
}

// Link diagnostics — surfaced in the heartbeat line.
volatile uint32_t meshRxAll = 0, meshRxQueued = 0, meshRxDup = 0,
                meshRxSelf = 0, meshRxBad = 0, meshRxShort = 0;
volatile uint32_t meshTxSent = 0, meshTxFail = 0, meshTxCbFail = 0;

void meshStats(uint32_t& rxAll, uint32_t& rxQueued, uint32_t& rxDup,
               uint32_t& rxSelf, uint32_t& rxBad,
               uint32_t& txSent, uint32_t& txFail, uint32_t& txCbFail) {
    rxAll = meshRxAll; rxQueued = meshRxQueued; rxDup = meshRxDup;
    rxSelf = meshRxSelf; rxBad = meshRxBad;
    txSent = meshTxSent; txFail = meshTxFail; txCbFail = meshTxCbFail;
}

void meshSniffRssi(const uint8_t* id, int8_t rssi) {
    uint32_t now = millis();
    int free_ = -1;
    for (int i = 0; i < MESH_SNIFF_SLOTS; i++) {
        if (sniffId[i][0] == 0 && sniffId[i][1] == 0 && sniffId[i][2] == 0) { free_ = i; break; }
        if (memcmp(sniffId[i], id, 3) == 0) {
            sniffRssi[i] = rssi; sniffTs[i] = now; return;
        }
    }
    int slot = free_;
    if (slot < 0) {
        uint32_t oldest = 0xFFFFFFFF;
        for (int i = 0; i < MESH_SNIFF_SLOTS; i++) {
            if (sniffTs[i] < oldest) { oldest = sniffTs[i]; slot = i; }
        }
    }
    if (slot < 0) return;
    memcpy(sniffId[slot], id, 3);
    sniffRssi[slot] = rssi;
    sniffTs[slot] = now;
}

int8_t meshLinkRssi() {
    uint32_t now = millis();
    int8_t best = 0;
    uint32_t bestTs = 0;
    for (int i = 0; i < MESH_SNIFF_SLOTS; i++) {
        if (sniffId[i][0] == 0 && sniffId[i][1] == 0 && sniffId[i][2] == 0) continue;
        if (now - sniffTs[i] > 10000) continue;
        if (sniffTs[i] > bestTs) { bestTs = sniffTs[i]; best = sniffRssi[i]; }
    }
    return best;
}

// ================================
// NVS persistence
// ================================
static void keyCtrSave() {
    Preferences p;
    p.begin("ouispy", false);
    p.putBytes("mk_rx", keyCtr, sizeof(keyCtr));
    p.end();
}

void mesh_loadConfig() {
    Preferences p;
    p.begin("ouispy", true);
    meshCfg.enabled = p.getBool("mesh_en", false);
    meshCfg.home    = p.getBool("mesh_home", false);
    meshCfg.channel = (uint8_t)p.getUChar("mesh_ch", 6);
    String key = p.getString("mesh_key", "");
    strlcpy(meshCfg.key, key.c_str(), sizeof(meshCfg.key));
    memset(keyCtr, 0, sizeof(keyCtr));
    if (p.getBytesLength("mk_rx") == sizeof(keyCtr)) p.getBytes("mk_rx", keyCtr, sizeof(keyCtr));
    keyTxCtr = p.getULong("mk_tx", 0);
    p.end();
    // One radio, two interfaces: in AP_STA the AP beacon follows the STA
    // channel, so a mesh channel of 12-13 would drag the portal AP onto a
    // band many client devices can't even see. Clamp the fleet to
    // universally visible channels.
    if (meshCfg.channel < MESH_CH_MIN || meshCfg.channel > MESH_CH_MAX) meshCfg.channel = 6;
}

void mesh_saveConfig() {
    Preferences p;
    p.begin("ouispy", false);
    p.putBool("mesh_en", meshCfg.enabled);
    p.putBool("mesh_home", meshCfg.home);
    p.putUChar("mesh_ch", meshCfg.channel);
    p.putString("mesh_key", meshCfg.key);
    p.end();
}

bool meshKeySet() { return meshCfg.key[0] != 0; }

void meshSetKey(const char* key) {
    if (strncmp(meshCfg.key, key, sizeof(meshCfg.key)) == 0) return;
    strlcpy(meshCfg.key, key, sizeof(meshCfg.key));
    // A new key starts a new trust epoch: forget every commander's counter
    // so a re-provisioned home (counter back at 0) is accepted again.
    memset(keyCtr, 0, sizeof(keyCtr));
    keyCtrSave();
    mesh_saveConfig();
}

bool meshIsRemoteUplink() { return meshCfg.enabled && !meshCfg.home; }

// ================================
// Dedup cache + roster
// ================================
// Returns true if this (origin, seq) was already handled within the TTL —
// i.e. the caller must drop the packet. Otherwise claims a slot and returns
// false. The whole table is searched for a hit before any slot is claimed,
// so a fresh entry stored behind a free or expired slot is still found.
static bool meshSeen(const uint8_t* origin, uint8_t seq, uint32_t now) {
    int freeSlot = -1, oldest = -1;
    uint32_t oldestAge = 0;
    for (int i = 0; i < MESH_SEEN_SIZE; i++) {
        if (!seenCache[i].live) { if (freeSlot < 0) freeSlot = i; continue; }
        uint32_t age = now - seenCache[i].ts;   // wrap-safe
        if (age >= MESH_SEEN_TTL_MS) { if (freeSlot < 0) freeSlot = i; continue; }
        if (seenCache[i].seq == seq && memcmp(seenCache[i].origin, origin, 6) == 0) return true;
        if (age >= oldestAge) { oldestAge = age; oldest = i; }
    }
    int slot = (freeSlot >= 0) ? freeSlot : oldest;
    if (slot < 0) return false;
    memcpy(seenCache[slot].origin, origin, 6);
    seenCache[slot].seq = seq;
    seenCache[slot].ts = now;
    seenCache[slot].live = true;
    return false;
}

// Returns true only when the ID claimed a NEW roster slot (a genuine join) —
// the caller answers with a burst so both sides' link LEDs light at the
// same moment instead of waiting for the next announce timer. The role is
// only ever taken from an announce: other packet types refresh presence
// without touching it.
static bool meshRosterUpdate(const uint8_t* id, bool hasRole, uint8_t role, uint32_t now) {
    int empty = -1;
    for (int i = 0; i < MESH_ROSTER_SIZE; i++) {
        if (roster[i].live && memcmp(roster[i].id, id, 3) == 0) {
            roster[i].lastSeen = now;
            if (hasRole) roster[i].role = role;
            roster[i].pkts++;
            return false;
        }
        if (!roster[i].live && empty < 0) empty = i;
    }
    int slot = empty;
    if (slot < 0) {  // full: steal the stalest
        uint32_t oldestAge = 0;
        for (int i = 0; i < MESH_ROSTER_SIZE; i++) {
            uint32_t age = now - roster[i].lastSeen;
            if (slot < 0 || age > oldestAge) { oldestAge = age; slot = i; }
        }
    }
    if (slot < 0) return false;
    memcpy(roster[slot].id, id, 3);
    roster[slot].lastSeen = now;
    roster[slot].role = hasRole ? role : 0xFF;
    roster[slot].pkts = 1;
    roster[slot].live = true;
    if (isSerialConnected()) {
        if (!hasRole) {
            Serial.printf("[MESH] node %02X%02X%02X joined\n", id[0], id[1], id[2]);
        } else {
            Serial.printf("[MESH] node %02X%02X%02X joined (%s)\n",
                          id[0], id[1], id[2], role ? "home" : "remote");
        }
    }
    return true;
}

uint8_t meshNodeCount() {
    uint32_t now = millis();
    uint8_t n = 0;
    for (int i = 0; i < MESH_ROSTER_SIZE; i++) {
        if (roster[i].live && now - roster[i].lastSeen < MESH_ROSTER_TIMEOUT) n++;
    }
    return n;
}

// Link LED truth: any frame heard from any node within the light window.
// Kept tighter than the roster timeout so a dead peer's LED goes dark
// ~4.5 s after the last frame instead of riding the management timeout.
bool meshLinkFresh() {
    uint32_t now = millis();
    for (int i = 0; i < MESH_ROSTER_SIZE; i++) {
        if (roster[i].live && now - roster[i].lastSeen < MESH_LIGHT_TIMEOUT_MS) return true;
    }
    return false;
}

void meshNodesJson(String& out) {
    uint32_t now = millis();
    out = "{\"nodes\":[";
    bool first = true;
    for (int i = 0; i < MESH_ROSTER_SIZE; i++) {
        if (!roster[i].live || now - roster[i].lastSeen >= MESH_ROSTER_TIMEOUT) continue;
        if (!first) out += ",";
        first = false;
        const MeshNode& n = roster[i];
        const char* role = (n.role == 0xFF) ? "unknown" : (n.role ? "home" : "remote");
        int8_t rssi = 0;
        for (int s = 0; s < MESH_SNIFF_SLOTS; s++) {
            if (memcmp(sniffId[s], n.id, 3) == 0 && now - sniffTs[s] < 10000) {
                rssi = sniffRssi[s];
                break;
            }
        }
        char idHex[7];
        snprintf(idHex, sizeof(idHex), "%02X%02X%02X", n.id[0], n.id[1], n.id[2]);
        out += "{\"id\":\"" + String(idHex) +
               "\",\"role\":\"" + role +
               "\",\"last\":" + String((now - n.lastSeen) / 1000) +
               ",\"pkts\":" + String(n.pkts) +
               ",\"rssi\":" + String(rssi) + "}";
    }
    out += "]}";
}

// ================================
// Packet builders
// ================================
static uint8_t meshBuildHeader(uint8_t* out, uint8_t type, uint8_t ttl) {
    out[0] = MESH_MAGIC0;
    out[1] = MESH_MAGIC1;
    out[2] = MESH_VERSION;
    out[3] = type;
    out[4] = ttl;
    out[5] = meshSeq++;
    memcpy(out + 6, selfMac, 6);
    return MESH_HDR_LEN;
}

static uint8_t meshBuildBodyId(uint8_t* out) {
    memcpy(out, selfId, 3);
    return 3;
}

// ================================
// TX queue
// ================================
static bool meshTxPush(const uint8_t* pkt, uint8_t len) {
    if (txCount >= MESH_TXQ_SIZE) return false;
    memcpy(txQ[txHead], pkt, len);
    txLen[txHead] = len;
    txHead = (txHead + 1) % MESH_TXQ_SIZE;
    txCount++;
    return true;
}

// Feed the next chunks of the outgoing command into the TX queue, leaving
// MESH_TXQ_RESERVE slots for announces, acks and relays. Chunk k carries
// bytes [180k, 180k+180); the final chunk carries the CRC of the payload.
static void meshPumpCommand() {
    while (outActive && txCount < MESH_TXQ_SIZE - MESH_TXQ_RESERVE) {
        uint16_t chunkLen = min((uint16_t)MESH_CMD_CHUNK, (uint16_t)(outLen - outOff));
        bool more = (outOff + chunkLen) < outLen;
        uint8_t pkt[MESH_PKT_MAX];
        uint8_t n = meshBuildHeader(pkt, MESH_T_COMMAND, MESH_TTL);
        n += meshBuildBodyId(pkt + n);
        pkt[n++] = outCmd;
        pkt[n++] = (uint8_t)(outCmdId & 0xFF);
        pkt[n++] = (uint8_t)(outCmdId >> 8);
        pkt[n++] = (uint8_t)(outOff & 0xFF);
        pkt[n++] = (uint8_t)(outOff >> 8);
        pkt[n++] = more ? 1 : 0;
        uint32_t crc = more ? 0 : outCrc;
        memcpy(pkt + n, &crc, 4); n += 4;
        if (chunkLen > 0) { memcpy(pkt + n, outBuf + outOff, chunkLen); n += chunkLen; }
        meshTxPush(pkt, n);
        outOff += chunkLen;
        if (!more) outActive = false;
    }
}

// Flush the TX queue — ONLY while the radio is parked on the mesh channel.
// Sending while a WiFi sweep owns the radio (or mid channel hop) fails at
// the driver level. Packets wait for the next BLE phase; the loop calls
// this every pass, so the delay is at most one phase (~2.2 s). A frame the
// driver rejects (typically a BLE coexistence collision) gets exactly one
// retry next flush instead of vanishing.
void meshFlushTxNow() {
    if (!meshStarted || !meshParked) return;
    meshPumpCommand();
    uint8_t bcast[6];
    memset(bcast, 0xFF, 6);
    static uint8_t retryBuf[MESH_PKT_MAX];
    static uint8_t retryLen = 0;
    if (retryLen > 0) {
        if (esp_now_send(bcast, retryBuf, retryLen) == ESP_OK) meshTxSent++;
        else meshTxFail++;
        retryLen = 0;
    }
    while (txCount > 0) {
        if (esp_now_send(bcast, txQ[txTail], txLen[txTail]) == ESP_OK) meshTxSent++;
        else {
            meshTxFail++;
            memcpy(retryBuf, txQ[txTail], txLen[txTail]);
            retryLen = txLen[txTail];
        }
        txTail = (txTail + 1) % MESH_TXQ_SIZE;
        txCount--;
    }
}

// Frames still to send, counting command chunks not yet in the queue.
uint8_t meshTxPending() {
    uint16_t left = 0;
    if (outActive) left = (uint16_t)((outLen - outOff + MESH_CMD_CHUNK - 1) / MESH_CMD_CHUNK);
    if (outActive && left == 0) left = 1;   // empty payload: one final chunk
    uint16_t total = txCount + left;
    return total > 255 ? 255 : (uint8_t)total;
}

void meshSelfIdHex(char out[7]) {
    uint8_t baseMac[6];
    esp_efuse_mac_get_default(baseMac);
    snprintf(out, 7, "%02X%02X%02X", baseMac[3], baseMac[4], baseMac[5]);
}

void meshQueueDetection(const char* mac, const char* ident, const char* desc,
                        int rssi, uint8_t dtype, bool wifiDomain) {
    if (!meshStarted) return;
    uint8_t pkt[MESH_PKT_MAX];
    uint8_t n = meshBuildHeader(pkt, MESH_T_DETECT, MESH_TTL);
    n += meshBuildBodyId(pkt + n);
    pkt[n++] = (uint8_t)(int8_t)rssi;
    pkt[n++] = dtype;
    pkt[n++] = wifiDomain ? 0x01 : 0x00;
    memset(pkt + n, 0, 18 + 18 + 40);
    memcpy(pkt + n, mac, min(strlen(mac) + 1, (size_t)18));    n += 18;
    memcpy(pkt + n, ident, min(strlen(ident) + 1, (size_t)18)); n += 18;
    memcpy(pkt + n, desc, min(strlen(desc) + 1, (size_t)40));   n += 40;

    if (!meshTxPush(pkt, n) && isSerialConnected()) {
        Serial.println("[MESH] TX queue full, detection not flooded");
    }
}

static void meshSendAnnounce() {
    uint8_t pkt[MESH_BYE_LEN + 1];
    uint8_t n = meshBuildHeader(pkt, MESH_T_ANNOUNCE, 1);
    n += meshBuildBodyId(pkt + n);
    pkt[n++] = meshCfg.home ? 1 : 0;
    if (!meshTxPush(pkt, n) && isSerialConnected()) {
        Serial.println("[MESH] TX queue full, announce dropped");
    }
}

uint16_t meshNewCmdId() {
    return (uint16_t)(random(1, 0x10000));
}

// ================================
// Fleet key (HMAC-SHA256 command signing)
// ================================
// tag = HMAC-SHA256(key, "OSMK" | cmd | cmdId(LE) | originId[3] | ctr(LE) | payload)[0:16]
static void meshSign(uint8_t cmd, uint16_t cmdId, const uint8_t* originId, uint32_t ctr,
                     const uint8_t* payload, uint16_t len, uint8_t tag[MESH_SIG_TAG_LEN]) {
    uint8_t hdr[4 + 1 + 2 + 3 + 4] = { 'O', 'S', 'M', 'K', cmd,
                                       (uint8_t)(cmdId & 0xFF), (uint8_t)(cmdId >> 8),
                                       originId[0], originId[1], originId[2],
                                       (uint8_t)ctr, (uint8_t)(ctr >> 8),
                                       (uint8_t)(ctr >> 16), (uint8_t)(ctr >> 24) };
    uint8_t mac[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char*)meshCfg.key, strlen(meshCfg.key));
    mbedtls_md_hmac_update(&ctx, hdr, sizeof(hdr));
    if (len) mbedtls_md_hmac_update(&ctx, payload, len);
    mbedtls_md_hmac_finish(&ctx, mac);
    mbedtls_md_free(&ctx);
    memcpy(tag, mac, MESH_SIG_TAG_LEN);
}

// Counters must strictly increase per commander; a capture replayed later
// carries an old counter and is refused.
static bool keyCtrFresh(const uint8_t* id, uint32_t ctr) {
    for (int i = 0; i < MESH_KEYCTR_SLOTS; i++) {
        if (memcmp(keyCtr[i].id, id, 3) == 0 && keyCtr[i].ctr != 0) return ctr > keyCtr[i].ctr;
    }
    return ctr > 0;
}

static void keyCtrStore(const uint8_t* id, uint32_t ctr) {
    int slot = -1, lowest = 0;
    for (int i = 0; i < MESH_KEYCTR_SLOTS; i++) {
        if (memcmp(keyCtr[i].id, id, 3) == 0 && keyCtr[i].ctr != 0) { slot = i; break; }
        if (keyCtr[i].ctr < keyCtr[lowest].ctr) lowest = i;
    }
    if (slot < 0) slot = lowest;   // free slots have ctr 0, so they are lowest
    memcpy(keyCtr[slot].id, id, 3);
    keyCtr[slot].ctr = ctr;
    keyCtrSave();
}

bool meshQueueCommand(uint8_t cmd, uint16_t cmdId, const uint8_t* payload, uint16_t len) {
    if (!meshStarted || outActive) return false;
    bool sign = meshKeySet();
    if (len > MESH_STAGE_MAX - (sign ? MESH_SIG_LEN : 0)) return false;
    if (len) memcpy(outBuf, payload, len);
    outCmd = cmd & 0x7F;
    if (sign) {
        keyTxCtr++;
        Preferences p;
        p.begin("ouispy", false);
        p.putULong("mk_tx", keyTxCtr);
        p.end();
        memcpy(outBuf + len, &keyTxCtr, 4);
        meshSign(outCmd, cmdId, selfId, keyTxCtr, outBuf, len, outBuf + len + 4);
        len += MESH_SIG_LEN;
        outCmd |= MESH_CMD_SIGNED;
    }
    outLen = len;
    outOff = 0;
    outCmdId = cmdId;
    outCrc = crc32_le(0, outBuf, outLen);
    outActive = true;
    meshPumpCommand();
    return true;
}

void meshSendAck(uint8_t cmd, uint16_t cmdId, uint8_t status, uint8_t count) {
    if (!meshStarted) return;
    uint8_t pkt[MESH_ACK_LEN];
    // Flooded like detections, so remotes beyond the commander's direct
    // reach still get their ack home. Older nodes accept any ttl.
    uint8_t n = meshBuildHeader(pkt, MESH_T_ACK, MESH_TTL);
    n += meshBuildBodyId(pkt + n);
    pkt[n++] = cmd & 0x7F;
    pkt[n++] = (uint8_t)(cmdId & 0xFF);
    pkt[n++] = (uint8_t)(cmdId >> 8);
    pkt[n++] = status;
    pkt[n++] = count;
    if (!meshTxPush(pkt, n) && isSerialConnected()) {
        Serial.println("[MESH] TX queue full, ack dropped");
    }
}

uint8_t meshCmdAckCount(uint16_t cmdId) {
    for (int i = 0; i < 4; i++) {
        if (acks[i].cmdId == cmdId) return acks[i].count;
    }
    return 0;
}

static void meshAckRecord(uint16_t cmdId, uint8_t status) {
    if (status != 0) return;   // a refusal is not a confirmation
    int free_ = -1;
    for (int i = 0; i < 4; i++) {
        if (acks[i].cmdId == cmdId) { acks[i].count++; return; }
        if (acks[i].count == 0) free_ = i;
    }
    int slot = (free_ >= 0) ? free_ : 0;
    acks[slot].cmdId = cmdId;
    acks[slot].count = 1;
}

// ================================
// Command reassembly (loop context)
// ================================
static void stageReset() {
    stageChunks = 0;
    stageActive = false;
    stageHaveFinal = false;
    stageTotal = 0;
}

static bool doneRecently(const uint8_t* origin, uint16_t cmdId, uint32_t now) {
    for (int i = 0; i < 4; i++) {
        if (doneCache[i].live && doneCache[i].cmdId == cmdId &&
            memcmp(doneCache[i].origin, origin, 3) == 0 &&
            now - doneCache[i].ts < MESH_STAGE_GAP_MS) return true;
    }
    return false;
}

static void doneRemember(const uint8_t* origin, uint16_t cmdId, uint32_t now) {
    int slot = 0;
    uint32_t oldestAge = 0;
    for (int i = 0; i < 4; i++) {
        if (!doneCache[i].live) { slot = i; break; }
        uint32_t age = now - doneCache[i].ts;
        if (age > oldestAge) { oldestAge = age; slot = i; }
    }
    memcpy(doneCache[slot].origin, origin, 3);
    doneCache[slot].cmdId = cmdId;
    doneCache[slot].ts = now;
    doneCache[slot].live = true;
}

// Verify (fleet key) and hand a complete, CRC-checked payload to main.cpp.
static void meshDeliverCommand(uint8_t wireCmd, uint16_t cmdId, const uint8_t* origin,
                               const uint8_t* buf, uint16_t total) {
    uint8_t cmd = wireCmd & 0x7F;
    bool isSigned = (wireCmd & MESH_CMD_SIGNED) != 0;
    if (!meshKeySet()) {
        if (isSigned) {
            if (isSerialConnected()) Serial.println("[MESH] signed command refused: no fleet key set on this node");
            meshSendAck(cmd, cmdId, 2, 0);
            return;
        }
        meshApplyCommand(cmd, cmdId, buf, total);
        return;
    }
    if (!isSigned || total < MESH_SIG_LEN) {
        if (isSerialConnected()) Serial.println("[MESH] unsigned command refused: this node requires the fleet key");
        meshSendAck(cmd, cmdId, 2, 0);
        return;
    }
    uint16_t appLen = total - MESH_SIG_LEN;
    uint32_t ctr;
    memcpy(&ctr, buf + appLen, 4);
    uint8_t tag[MESH_SIG_TAG_LEN];
    meshSign(cmd, cmdId, origin, ctr, buf, appLen, tag);
    uint8_t diff = 0;
    for (int i = 0; i < MESH_SIG_TAG_LEN; i++) diff |= tag[i] ^ buf[appLen + 4 + i];
    if (diff) {
        if (isSerialConnected()) Serial.println("[MESH] command refused: bad fleet key signature");
        meshSendAck(cmd, cmdId, 2, 0);
        return;
    }
    if (!keyCtrFresh(origin, ctr)) {
        if (isSerialConnected()) Serial.println("[MESH] command refused: replayed (old counter)");
        meshSendAck(cmd, cmdId, 3, 0);
        return;
    }
    keyCtrStore(origin, ctr);
    meshApplyCommand(cmd, cmdId, buf, appLen);
}

static void meshHandleCommand(const uint8_t* pkt, uint8_t pktLen) {
    const uint8_t* body = pkt + MESH_HDR_LEN;
    const uint8_t* origin = body;   // nodeId
    uint8_t  cmd    = body[3];
    uint16_t cmdId  = body[4] | (body[5] << 8);
    uint16_t offset = body[6] | (body[7] << 8);
    bool     more   = body[8] != 0;
    uint32_t crc;
    memcpy(&crc, body + 9, 4);
    const uint8_t* data = body + 13;
    uint16_t len = pktLen - (MESH_HDR_LEN + 13);

    uint32_t now = millis();
    if (doneRecently(origin, cmdId, now)) return;   // already applied (late resend)

    if (offset % MESH_CMD_CHUNK != 0 || len > MESH_CMD_CHUNK ||
        offset > MESH_STAGE_MAX || len > MESH_STAGE_MAX - offset ||
        (more && len != MESH_CMD_CHUNK)) {
        if (isSerialConnected()) Serial.println("[MESH] command chunk malformed, dropped");
        return;
    }

    if (!stageActive || stageCmdId != cmdId || memcmp(stageOrigin, origin, 3) != 0 ||
        now - stageLastChunk > MESH_STAGE_GAP_MS) {
        stageReset();
        stageActive = true;
        stageCmdId = cmdId;
        stageCmd = cmd;
        memcpy(stageOrigin, origin, 3);
    }
    stageLastChunk = now;

    if (len) memcpy(stageBuf + offset, data, len);
    stageChunks |= (1UL << (offset / MESH_CMD_CHUNK));
    if (!more) {
        stageHaveFinal = true;
        stageTotal = offset + len;
        stageCrc = crc;
    }
    if (!stageHaveFinal) return;

    // Complete once every chunk up to the final one is in.
    uint16_t lastIdx = stageTotal == 0 ? 0 : (stageTotal - 1) / MESH_CMD_CHUNK;
    uint32_t need = (lastIdx >= 31) ? 0xFFFFFFFFUL : ((1UL << (lastIdx + 1)) - 1);
    if ((stageChunks & need) != need) return;   // keep waiting for stragglers

    if (crc32_le(0, stageBuf, stageTotal) != stageCrc) {
        if (isSerialConnected()) Serial.println("[MESH] command CRC mismatch, discarded");
        stageReset();
        return;
    }
    uint8_t wireCmd = stageCmd;
    uint16_t total = stageTotal;
    uint8_t from[3];
    memcpy(from, stageOrigin, 3);
    stageReset();
    doneRemember(from, cmdId, now);
    meshDeliverCommand(wireCmd, cmdId, from, stageBuf, total);
}

// ================================
// ESP-NOW callbacks (WiFi driver task context — keep cheap, no heap)
// ================================
static void meshRecvCb(const uint8_t* senderMac, const uint8_t* data, int len) {
    meshRxAll++;
    if (!meshStarted || len < MESH_MIN_LEN) { meshRxShort++; return; }
    if (data[0] != MESH_MAGIC0 || data[1] != MESH_MAGIC1) { meshRxBad++; return; }

    if (data[2] != MESH_VERSION) { meshRxBad++; return; }
    uint8_t type = data[3];
    if (type == MESH_T_ANNOUNCE && len < MESH_MIN_LEN + 1) { meshRxShort++; return; }
    if (type == MESH_T_DETECT && len < MESH_DETECT_LEN) { meshRxShort++; return; }
    if (type == MESH_T_COMMAND && len < MESH_CMD_MIN) { meshRxShort++; return; }
    if (type == MESH_T_ACK && len < MESH_ACK_LEN) { meshRxShort++; return; }
    if (type == MESH_T_BYE && len < MESH_BYE_LEN) { meshRxShort++; return; }
    if (memcmp(data + 6, selfMac, 6) == 0) { meshRxSelf++; return; }  // our own packet relayed back

    if (meshSeen(data + 6, data[5], millis())) { meshRxDup++; return; }

    portENTER_CRITICAL(&rxMux);
    if (rxCount < MESH_RXQ_SIZE) {
        uint8_t l = (uint8_t)min(len, MESH_PKT_MAX);
        memcpy(rxQ[rxHead], data, l);
        rxLen[rxHead] = l;
        rxHead = (rxHead + 1) % MESH_RXQ_SIZE;
        rxCount++;
        meshRxQueued++;
    }
    portEXIT_CRITICAL(&rxMux);
}

static void meshSendCb(const uint8_t* mac_addr, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) { sendFailures++; meshTxCbFail++; }
}

// ================================
// Init / teardown
// ================================
bool mesh_init() {
    if (meshStarted || !meshCfg.enabled) return meshStarted;

    esp_wifi_get_mac(WIFI_IF_STA, selfMac);
    // Stable node ID = factory eFuse MAC tail — immune to per-boot stealth
    // randomization and NVS wipes; this is what the roster, JSON and UI key on.
    uint8_t baseMac[6];
    esp_efuse_mac_get_default(baseMac);
    memcpy(selfId, baseMac + 3, 3);

    if (esp_now_init() != ESP_OK) {
        if (isSerialConnected()) Serial.println("[MESH] esp_now_init failed");
        return false;
    }

    // LR protocol in the STA mask on every node (both ends must have it).
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                                      WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_1M_L);
#ifdef MESH_ESPNOW_LR_RATE
    // Opt-in (build flag): send ESP-NOW itself at the LR PHY rate. A fleet-
    // wide change — every node must run the same setting. Without it ESP-NOW
    // uses the driver's default rate.
    esp_wifi_config_espnow_rate(WIFI_IF_STA, (wifi_phy_rate_t)MESH_ESPNOW_LR_RATE);
#endif
    // NOTE: deliberately no esp_wifi_set_ps()/set_max_tx_power() here. Both
    // abort inside pm_set_sleep_type from wifi_set_channel_process on this
    // core/IDF combo (bootloop, found the hard way). For a disconnected STA
    // the defaults are effectively max anyway.

    esp_now_register_recv_cb(meshRecvCb);
    esp_now_register_send_cb(meshSendCb);

    // Single broadcast peer on the current channel: meshTick only flushes
    // the TX queue while the radio is parked on the mesh channel, so TX
    // always lands there with no per-peer channel pinning (pinned peers fail
    // to send mid-sweep — observed as tx fail counter climbing).
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memset(peer.peer_addr, 0xFF, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        if (isSerialConnected()) Serial.println("[MESH] add broadcast peer failed");
        esp_now_deinit();
        return false;
    }

    esp_wifi_set_channel(meshCfg.channel, WIFI_SECOND_CHAN_NONE);
    meshParked = true;

    meshStarted = true;
    lastAnnounce = millis();
    lastTrafficAt = millis();
    if (isSerialConnected()) {
        Serial.printf("[MESH] ESP-NOW LR up: %s node, channel %d, id %02X%02X%02X, fleet key %s\n",
                      meshCfg.home ? "HOME" : "REMOTE", meshCfg.channel,
                      selfId[0], selfId[1], selfId[2], meshKeySet() ? "set" : "OFF");
    }
    return true;
}

void mesh_deinit() {
    if (!meshStarted) return;
    // Best-effort goodbye: peers drop us instantly instead of at the
    // silence threshold. Only leaves if the radio is parked on the mesh
    // channel right now — a mid-sweep bye stays queued and dies with the
    // stack, which the silence threshold covers anyway.
    uint8_t pkt[MESH_BYE_LEN];
    uint8_t n = meshBuildHeader(pkt, MESH_T_BYE, 1);
    n += meshBuildBodyId(pkt + n);
    pkt[n++] = 0;
    meshTxPush(pkt, n);
    meshFlushTxNow();
    delay(80);
    meshStarted = false;
    meshParked = false;
    outActive = false;
    stageReset();
    burstRemaining = 0;
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
}

void meshParkChannel() {
    if (!meshStarted) return;
    esp_wifi_set_channel(meshCfg.channel, WIFI_SECOND_CHAN_NONE);
    meshParked = true;
}

void meshUnparkChannel() { meshParked = false; }

static void meshRelay(uint8_t* pkt, uint8_t len) {
    if (pkt[4] > 1 && txCount < MESH_TXQ_SIZE) {
        pkt[4]--;
        if (!meshTxPush(pkt, len)) pkt[4]++;  // no slot: don't burn the ttl
    }
}

static void meshStartBurst(uint32_t now) {
    lastAnnounce = now;
    burstRemaining = MESH_BURST_LEN;
    burstNextAt = now;
}

// ================================
// loop()-context tick
// ================================
void meshTick() {
    if (!meshStarted) return;
    uint32_t now = millis();

    // Drain RX ring.
    while (true) {
        uint8_t pkt[MESH_PKT_MAX];
        uint8_t len = 0;
        portENTER_CRITICAL(&rxMux);
        if (rxCount > 0) {
            memcpy(pkt, rxQ[rxTail], rxLen[rxTail]);
            len = rxLen[rxTail];
            rxTail = (rxTail + 1) % MESH_RXQ_SIZE;
            rxCount--;
        }
        portEXIT_CRITICAL(&rxMux);
        if (len == 0) break;
        lastTrafficAt = now;

        uint8_t type = pkt[3];
        const uint8_t* nodeId = pkt + MESH_HDR_LEN;
        bool isAnnounce = (type == MESH_T_ANNOUNCE);
        bool isNewNode = meshRosterUpdate(nodeId, isAnnounce,
                                          isAnnounce ? pkt[MESH_HDR_LEN + 3] : 0xFF, now);
        if (isNewNode) {
            // Link handshake: answer a new neighbor with a full burst so
            // both sides' rosters (and link LEDs) fill within one frame of
            // each other instead of waiting for the next announce timer.
            meshStartBurst(now);
        }

        switch (type) {
        case MESH_T_DETECT: {
            const uint8_t* body = pkt + MESH_HDR_LEN;
            int8_t rssi   = (int8_t)body[3];
            uint8_t dtype = body[4];
            bool wifiDom  = (body[5] & 0x01) != 0;
            char mac[19], ident[19], desc[41];
            strlcpy(mac,   (const char*)(body + 6),  sizeof(mac));
            strlcpy(ident, (const char*)(body + 24), sizeof(ident));
            strlcpy(desc,  (const char*)(body + 42), sizeof(desc));

            // Desc arrives sender-truncated to 40 chars; append the mesh
            // provenance inside main.cpp's 64-char description budget.
            char full[64];
            snprintf(full, sizeof(full), "%.40s [mesh %02X%02X%02X]",
                     desc, nodeId[0], nodeId[1], nodeId[2]);
            meshReport(mac, ident, full, rssi, dtype, wifiDom);
            meshRelay(pkt, len);
            break;
        }
        case MESH_T_COMMAND:
            meshRelay(pkt, len);   // relay first: a reboot command still spreads
            meshHandleCommand(pkt, len);
            break;
        case MESH_T_ACK:
            meshAckRecord(pkt[MESH_HDR_LEN + 4] | (pkt[MESH_HDR_LEN + 5] << 8),
                          pkt[MESH_HDR_LEN + 6]);
            meshRelay(pkt, len);
            break;
        case MESH_T_BYE:
            // Controlled shutdown: drop the origin right now so the link
            // LED goes dark instantly instead of at the silence threshold.
            // (Power loss has no bye — that's what the threshold is for.)
            for (int i = 0; i < MESH_ROSTER_SIZE; i++) {
                if (roster[i].live && memcmp(roster[i].id, nodeId, 3) == 0) {
                    roster[i].live = false;
                    if (isSerialConnected()) {
                        Serial.printf("[MESH] node %02X%02X%02X offline (bye)\n",
                                      nodeId[0], nodeId[1], nodeId[2]);
                    }
                }
            }
            break;
        case MESH_T_ANNOUNCE:
        default:
            break;  // announces carry ttl=1 and are never relayed
        }
    }

    // Stale partial transfer cleanup.
    if (stageActive && now - stageLastChunk > MESH_STAGE_GAP_MS) stageReset();

    // Channel auto-heal: silent too long while meshing → walk the mesh
    // channels (1-11, the same range the config allows), listening on each.
    // Any mesh traffic adopts that channel permanently. The loop() bypasses
    // the scan phase machine while this runs so nothing fights the changes.
    if (healAllowed && !healActive && meshNodeCount() == 0 && now - lastTrafficAt > MESH_HEAL_AFTER_MS) {
        healActive = true;
        healCh = MESH_CH_MIN;
        healStepAt = now;
        esp_wifi_set_channel(healCh, WIFI_SECOND_CHAN_NONE);
        meshParked = true;
        if (isSerialConnected()) Serial.println("[MESH] alone >2min — sweeping channels for the fleet");
    }
    if (healActive) {
        if (now - lastTrafficAt <= MESH_HEAL_DWELL_MS && now - healStepAt > 500) {
            meshCfg.channel = healCh;
            mesh_saveConfig();
            healActive = false;
            meshStartBurst(now);
            if (isSerialConnected()) {
                Serial.printf("[MESH] fleet found on channel %d — adopted and saved\n", healCh);
            }
        } else if (now - healStepAt >= MESH_HEAL_DWELL_MS) {
            healCh = (healCh >= MESH_CH_MAX) ? MESH_CH_MIN : healCh + 1;
            healStepAt = now;
            esp_wifi_set_channel(healCh, WIFI_SECOND_CHAN_NONE);
        }
    }

    // Flush the TX queue (parked-gated; no-op during sweeps).
    static uint32_t lastFailReport = 0;
    meshFlushTxNow();
    if (sendFailures > 0 && now - lastFailReport > 10000) {
        lastFailReport = now;
        if (isSerialConnected()) {
            Serial.printf("[MESH] %lu TX unacknowledged (broadcast ACK is best-effort)\n",
                          (unsigned long)sendFailures);
        }
        sendFailures = 0;
    }

    // Presence bursts: MESH_BURST_LEN frames MESH_BURST_GAP_MS apart. Each
    // lands in a peer's mesh window roughly half the time, so a burst makes
    // per-period delivery near certain and the link LED's freshness window
    // can be a few seconds: a yanked node shows offline in ~4.5 s and a
    // live link never flickers.
    if (burstRemaining > 0 && now >= burstNextAt) {
        meshSendAnnounce();
        burstRemaining--;
        burstNextAt = now + MESH_BURST_GAP_MS;
    }
    unsigned long announceInterval = (meshNodeCount() > 0) ? MESH_ANNOUNCE_MS
                                                           : MESH_ANNOUNCE_LONE_MS;
    if (now - lastAnnounce >= announceInterval) meshStartBurst(now);

    // Prune silent nodes.
    static unsigned long lastPrune = 0;
    if (now - lastPrune >= 3000) {
        lastPrune = now;
        for (int i = 0; i < MESH_ROSTER_SIZE; i++) {
            if (roster[i].live && now - roster[i].lastSeen >= MESH_ROSTER_TIMEOUT) {
                if (isSerialConnected()) {
                    Serial.printf("[MESH] node %02X%02X%02X offline\n",
                                  roster[i].id[0], roster[i].id[1], roster[i].id[2]);
                }
                roster[i].live = false;
            }
        }
    }
}
