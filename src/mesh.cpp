#include "mesh.h"
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Preferences.h>
#include <rom/crc.h>
#include <string.h>
#include <esp_mac.h>

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
#define MESH_PKT_MAX    250
#define MESH_BODY       MESH_HDR_LEN
#define MESH_DETECT_LEN (MESH_HDR_LEN + 3 + 1 + 1 + 1 + 18 + 18 + 40)
#define MESH_CMD_MIN    (MESH_HDR_LEN + 3 + 1 + 2 + 2 + 1 + 4)
#define MESH_ACK_LEN    (MESH_HDR_LEN + 3 + 1 + 2 + 1 + 1)
#define MESH_BYE_LEN    (MESH_HDR_LEN + 3 + 1)
#define MESH_CMD_CHUNK  180
#define MESH_STAGE_MAX  4096
#define MESH_STAGE_SLOT 16
#define MESH_STAGE_BITMAP (MESH_STAGE_MAX / MESH_STAGE_SLOT / 8)
#define MESH_STAGE_GAP_MS 60000UL

#define MESH_TXQ_SIZE   8    // deep queues blast too many frames at once when
                             // a window opens and wedge the driver (fails +
                             // RX silence) — 8 caps the per-flush burst
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

MeshConfig meshCfg = { false, false, 6 };

static bool meshStarted = false;
static bool meshParked = false;
static uint8_t selfMac[6] = {0};
static uint8_t selfId[3] = {0};   // factory MAC tail — the stable node ID
static uint8_t meshSeq = 0;

// (origin, seq) duplicate cache — drops re-flooded copies within the TTL
// window. 32 entries and a 60 s TTL mean one origin can flood up to 256
// packets per window before its seq wraps onto a live entry; alert-rate
// cooldowns make that unreachable in practice.
struct MeshSeen { uint8_t origin[6]; uint8_t seq; uint32_t ts; bool live; };
static MeshSeen seenCache[MESH_SEEN_SIZE];

// TX queue: loop-context only (meshQueueDetection / meshQueueCommand /
// relay / acks), so no locking. Deep enough for OTA-class bursts.
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

// Ack counters for the last few command batches.
struct MeshAck { uint16_t cmdId; uint8_t count; };
static MeshAck acks[4];

// Command reassembly staging.
static uint8_t  stageBuf[MESH_STAGE_MAX];
static uint8_t  stageBitmap[MESH_STAGE_BITMAP];
static uint16_t stageContig = 0;
static uint16_t stageCmdId = 0;
static uint8_t  stageCmd = 0;
static uint32_t stageLastChunk = 0;
static bool     stageActive = false;

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
// moved) sweeps every channel listening for mesh traffic, adopts the
// channel it finds the fleet on, and persists it. Without this, a channel
// mismatch is a silent permanent partition.
static bool healActive = false;
static uint8_t healCh = 1;
static unsigned long healStepAt = 0;
static unsigned long lastTrafficAt = 0;
#define MESH_HEAL_AFTER_MS   120000UL
#define MESH_HEAL_DWELL_MS   3000UL

bool meshHealing() { return healActive; }
static bool healAllowed = true;
void meshSetHealAllowed(bool allowed) { healAllowed = allowed; }

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
void mesh_loadConfig() {
    Preferences p;
    p.begin("ouispy", true);
    meshCfg.enabled = p.getBool("mesh_en", false);
    meshCfg.home    = p.getBool("mesh_home", false);
    meshCfg.channel = (uint8_t)p.getUChar("mesh_ch", 6);
    p.end();
    // One radio, two interfaces: in AP_STA the AP beacon follows the STA
    // channel, so a mesh channel of 12-13 would drag the portal AP onto a
    // band many client devices can't even see. Clamp the fleet to
    // universally visible channels.
    if (meshCfg.channel < 1 || meshCfg.channel > 11) meshCfg.channel = 6;
}

void mesh_saveConfig() {
    Preferences p;
    p.begin("ouispy", false);
    p.putBool("mesh_en", meshCfg.enabled);
    p.putBool("mesh_home", meshCfg.home);
    p.putUChar("mesh_ch", meshCfg.channel);
    p.end();
}

bool meshIsRemoteUplink() { return meshCfg.enabled && !meshCfg.home; }

// ================================
// Dedup cache + roster
// ================================
// Returns true if this (origin, seq) was already handled within the TTL —
// i.e. the caller must drop the packet. Otherwise claims/refreshes a slot
// and returns false.
static bool meshSeen(const uint8_t* origin, uint8_t seq, uint32_t now) {
    int stale = -1;
    uint32_t oldestTs = 0xFFFFFFFF;
    int oldest = -1;
    for (int i = 0; i < MESH_SEEN_SIZE; i++) {
        if (!seenCache[i].live) { stale = i; break; }
        if (now - seenCache[i].ts > MESH_SEEN_TTL_MS) { stale = i; break; }
        if (seenCache[i].ts < oldestTs) { oldestTs = seenCache[i].ts; oldest = i; }
        if (seenCache[i].seq == seq && memcmp(seenCache[i].origin, origin, 6) == 0) {
            if (now - seenCache[i].ts < MESH_SEEN_TTL_MS) return true;
            stale = i;  // expired hit — refresh below
            break;
        }
    }
    int slot = (stale >= 0) ? stale : oldest;
    if (slot < 0) return false;  // table full of fresh foreign entries: accept
    memcpy(seenCache[slot].origin, origin, 6);
    seenCache[slot].seq = seq;
    seenCache[slot].ts = now;
    seenCache[slot].live = true;
    return false;
}

// Returns true only when the ID claimed a NEW roster slot (a genuine join) —
// the caller answers with a burst so both sides' link LEDs light at the
// same moment instead of waiting for the next announce timer.
static bool meshRosterUpdate(const uint8_t* id, uint8_t role, uint32_t now) {
    int empty = -1;
    for (int i = 0; i < MESH_ROSTER_SIZE; i++) {
        if (roster[i].live && memcmp(roster[i].id, id, 3) == 0) {
            roster[i].lastSeen = now;
            roster[i].role = role;
            roster[i].pkts++;
            return false;
        }
        if (!roster[i].live && empty < 0) empty = i;
    }
    int slot = empty;
    if (slot < 0) {  // full: steal the stalest
        uint32_t oldest = 0xFFFFFFFF;
        for (int i = 0; i < MESH_ROSTER_SIZE; i++) {
            if (roster[i].lastSeen < oldest) { oldest = roster[i].lastSeen; slot = i; }
        }
    }
    if (slot < 0) return false;
    memcpy(roster[slot].id, id, 3);
    roster[slot].lastSeen = now;
    roster[slot].role = role;
    roster[slot].pkts = 1;
    roster[slot].live = true;
    if (isSerialConnected()) {
        if (role == 0xFF) {
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

// Flush the TX queue — ONLY while the radio is parked on the mesh channel.
// Sending while a WiFi sweep owns the radio (or mid channel hop) fails at
// the driver level. Packets wait for the next BLE phase; the loop calls
// this every pass, so the delay is at most one phase (~2.2 s). A frame the
// driver rejects (typically a BLE coexistence collision) gets exactly one
// retry next flush instead of vanishing.
void meshFlushTxNow() {
    if (!meshStarted || !meshParked) return;
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

uint8_t meshTxPending() { return txCount; }

void meshSelfIdHex(char out[7]) {
    uint8_t baseMac[6];
    esp_efuse_mac_get_default(baseMac);
    snprintf(out, 7, "%02X%02X%02X", baseMac[3], baseMac[4], baseMac[5]);
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

void meshQueueDetection(const char* mac, const char* ident, const char* desc,
                        int rssi, uint8_t dtype, bool wifiDomain) {
    if (!meshStarted) return;
    uint8_t pkt[MESH_PKT_MAX];
    uint8_t n = meshBuildHeader(pkt, MESH_T_DETECT, MESH_TTL);
    n += meshBuildBodyId(pkt + n);
    pkt[n++] = (uint8_t)(int8_t)rssi;
    pkt[n++] = dtype;
    pkt[n++] = wifiDomain ? 0x01 : 0x00;
    memcpy(pkt + n, mac, min(strlen(mac) + 1, (size_t)18));    n += 18;
    memcpy(pkt + n, ident, min(strlen(ident) + 1, (size_t)18)); n += 18;
    memcpy(pkt + n, desc, min(strlen(desc) + 1, (size_t)40));   n += 40;

    // Claim (origin, seq) now so a relayed copy coming back to us is dropped
    // by the dedup cache before it can re-trigger local alerts.
    meshSeen(selfMac, pkt[5], millis());

    if (!meshTxPush(pkt, n) && isSerialConnected()) {
        Serial.println("[MESH] TX queue full, detection not flooded");
    }
}

static void meshSendAnnounce() {
    uint8_t pkt[MESH_BYE_LEN + 1];
    uint8_t n = meshBuildHeader(pkt, MESH_T_ANNOUNCE, 1);
    n += meshBuildBodyId(pkt + n);
    pkt[n++] = meshCfg.home ? 1 : 0;
    meshSeen(selfMac, pkt[5], millis());
    if (!meshTxPush(pkt, n) && isSerialConnected()) {
        Serial.println("[MESH] TX queue full, announce dropped");
    }
}

uint16_t meshNewCmdId() {
    return (uint16_t)(random(1, 0x10000));
}

bool meshQueueCommand(uint8_t cmd, uint16_t cmdId, const uint8_t* payload, uint16_t len) {
    if (!meshStarted || len > MESH_STAGE_MAX) return false;
    if (len == 0) payload = (const uint8_t*)"";  // crc32 of an empty payload
    for (uint16_t off = 0; off < len || (len == 0 && off == 0); off += MESH_CMD_CHUNK) {
        uint16_t chunkLen = min((uint16_t)MESH_CMD_CHUNK, (uint16_t)(len - off));
        bool more = (off + chunkLen) < len;
        uint8_t pkt[MESH_PKT_MAX];
        uint8_t n = meshBuildHeader(pkt, MESH_T_COMMAND, MESH_TTL);
        n += meshBuildBodyId(pkt + n);
        pkt[n++] = cmd;
        pkt[n++] = (uint8_t)(cmdId & 0xFF);
        pkt[n++] = (uint8_t)(cmdId >> 8);
        pkt[n++] = (uint8_t)(off & 0xFF);
        pkt[n++] = (uint8_t)(off >> 8);
        pkt[n++] = more ? 1 : 0;
        uint32_t crc = (more) ? 0 : crc32_le(0, payload, len);
        memcpy(pkt + n, &crc, 4); n += 4;
        if (chunkLen > 0) { memcpy(pkt + n, payload + off, chunkLen); n += chunkLen; }
        meshSeen(selfMac, pkt[5], millis());
        if (!meshTxPush(pkt, n)) {
            if (isSerialConnected()) Serial.println("[MESH] TX queue full, command truncated");
            return false;
        }
        if (len == 0) break;
    }
    return true;
}

void meshSendAck(uint8_t cmd, uint16_t cmdId, uint8_t status, uint8_t count) {
    if (!meshStarted) return;
    uint8_t pkt[MESH_ACK_LEN];
    uint8_t n = meshBuildHeader(pkt, MESH_T_ACK, 1);
    n += meshBuildBodyId(pkt + n);
    pkt[n++] = cmd;
    pkt[n++] = (uint8_t)(cmdId & 0xFF);
    pkt[n++] = (uint8_t)(cmdId >> 8);
    pkt[n++] = status;
    pkt[n++] = count;
    meshSeen(selfMac, pkt[5], millis());
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

static void meshAckRecord(uint16_t cmdId) {
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
    memset(stageBitmap, 0, sizeof(stageBitmap));
    stageContig = 0;
    stageActive = false;
}

static bool stageSlotReceived(uint16_t slot) { return stageBitmap[slot / 8] & (1 << (slot % 8)); }
static void stageSlotMark(uint16_t slot) { stageBitmap[slot / 8] |= (1 << (slot % 8)); }

static void meshHandleCommand(const uint8_t* pkt, uint8_t pktLen) {
    const uint8_t* body = pkt + MESH_HDR_LEN;
    uint8_t  cmd    = body[3];
    uint16_t cmdId  = body[4] | (body[5] << 8);
    uint16_t offset = body[6] | (body[7] << 8);
    bool     more   = body[8] != 0;
    uint32_t crc;
    memcpy(&crc, body + 9, 4);
    const uint8_t* data = body + 13;
    uint16_t len = pktLen - (MESH_HDR_LEN + 13);

    uint32_t now = millis();
    if (!stageActive || stageCmdId != cmdId || now - stageLastChunk > MESH_STAGE_GAP_MS) {
        stageReset();
        stageActive = true;
        stageCmdId = cmdId;
        stageCmd = cmd;
    }
    stageLastChunk = now;

    if (offset > MESH_STAGE_MAX || len > MESH_STAGE_MAX - offset) {
        if (isSerialConnected()) Serial.println("[MESH] command chunk out of range, dropped");
        stageReset();
        return;
    }
    memcpy(stageBuf + offset, data, len);
    for (uint16_t s = offset / MESH_STAGE_SLOT; s <= (offset + len - 1) / MESH_STAGE_SLOT; s++) {
        stageSlotMark(s);
    }
    while (stageContig < MESH_STAGE_MAX && stageSlotReceived(stageContig / MESH_STAGE_SLOT)) {
        stageContig += MESH_STAGE_SLOT;
    }

    if (more) return;

    uint16_t total = offset + len;
    if (stageContig < total) {
        if (isSerialConnected()) Serial.println("[MESH] command chunks missing, transfer incomplete");
        stageReset();
        return;
    }
    if (crc32_le(0, stageBuf, total) != crc) {
        if (isSerialConnected()) Serial.println("[MESH] command CRC mismatch, discarded");
        stageReset();
        return;
    }
    uint8_t appliedCmd = stageCmd;
    stageReset();
    meshApplyCommand(appliedCmd, cmdId, stageBuf, total);
}

// ================================
// ESP-NOW callbacks (WiFi driver task context — keep cheap, no heap)
// ================================
static void meshRecvCb(const uint8_t* senderMac, const uint8_t* data, int len) {
    meshRxAll++;
    if (!meshStarted || len < MESH_HDR_LEN) { meshRxShort++; return; }
    if (data[0] != MESH_MAGIC0 || data[1] != MESH_MAGIC1) { meshRxBad++; return; }

    if (data[2] != MESH_VERSION) { meshRxBad++; return; }
    uint8_t type = data[3];
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

    // LR mode: proprietary long-range PHY rate. Needs both ends enabled,
    // which is guaranteed here — every node runs this same init.
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                                      WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_1M_L);
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
        Serial.printf("[MESH] ESP-NOW LR up: %s node, channel %d, id %02X%02X%02X\n",
                      meshCfg.home ? "HOME" : "REMOTE", meshCfg.channel,
                      selfId[0], selfId[1], selfId[2]);
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
    meshSeen(selfMac, pkt[5], millis());
    meshTxPush(pkt, n);
    meshFlushTxNow();
    delay(80);
    meshStarted = false;
    meshParked = false;
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
        bool isNewNode = meshRosterUpdate(nodeId,
                             (type == MESH_T_ANNOUNCE) ? pkt[MESH_HDR_LEN + 3] : 0xFF, now);
        if (isNewNode) {
            // Link handshake: answer a new neighbor with a full burst so
            // both sides' rosters (and link LEDs) fill within one frame of
            // each other instead of waiting for the next announce timer.
            lastAnnounce = now;
            burstRemaining = MESH_BURST_LEN - 1;
            burstNextAt = now;
        }

        switch (type) {
        case MESH_T_DETECT: {
            const uint8_t* body = pkt + MESH_HDR_LEN;
            int8_t rssi   = (int8_t)body[3];
            uint8_t dtype = body[4];
            bool wifiDom  = (body[5] & 0x01) != 0;
            const char* mac   = (const char*)(body + 6);
            const char* ident = (const char*)(body + 24);
            const char* desc  = (const char*)(body + 42);

            // Desc arrives sender-truncated to 40 chars; append the mesh
            // provenance inside main.cpp's 64-char description budget.
            char full[64];
            snprintf(full, sizeof(full), "%.40s [mesh %02X%02X%02X]",
                     desc, nodeId[0], nodeId[1], nodeId[2]);
            meshReport(mac, ident, full, rssi, dtype, wifiDom);

            // Flood on while hops remain.
            if (pkt[4] > 1 && txCount < MESH_TXQ_SIZE) {
                pkt[4]--;
                if (!meshTxPush(pkt, len)) pkt[4]++;  // no slot: don't burn the ttl
            }
            break;
        }
        case MESH_T_COMMAND:
            if (pkt[4] > 1 && txCount < MESH_TXQ_SIZE) {  // relay like detections
                pkt[4]--;
                if (!meshTxPush(pkt, len)) pkt[4]++;
            }
            meshHandleCommand(pkt, len);
            break;
        case MESH_T_ACK:
            meshAckRecord(pkt[MESH_HDR_LEN + 4] | (pkt[MESH_HDR_LEN + 5] << 8));
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

    // Channel auto-heal: silent too long while meshing → walk all 13
    // channels, listening on each. Any mesh traffic adopts that channel
    // permanently. The loop() bypasses the scan phase machine while this
    // runs so nothing fights the channel changes.
    if (healAllowed && !healActive && meshNodeCount() == 0 && now - lastTrafficAt > MESH_HEAL_AFTER_MS) {
        healActive = true;
        healCh = 1;
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
            lastAnnounce = now;
            burstRemaining = MESH_BURST_LEN - 1;
            burstNextAt = now;
            if (isSerialConnected()) {
                Serial.printf("[MESH] fleet found on channel %d — adopted and saved\n", healCh);
            }
        } else if (now - healStepAt >= MESH_HEAL_DWELL_MS) {
            healCh = (healCh >= 13) ? 1 : healCh + 1;
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

    // Presence bursts: five frames 150 ms apart, each caught ~50% of the
    // time while the peer sits in a mesh window — a burst lifts per-cycle
    // delivery to ~97%, so the link LED's freshness window can be a few
    // seconds. Result: a yanked node shows as offline in ~4.5 s, and a live
    // link never flickers. Airtime cost is ~10 ms of LR frame per 2 s.
    if (burstRemaining > 0 && now >= burstNextAt) {
        meshSendAnnounce();
        burstRemaining--;
        burstNextAt = now + MESH_BURST_GAP_MS;
    }
    unsigned long announceInterval = (meshNodeCount() > 0) ? MESH_ANNOUNCE_MS
                                                           : MESH_ANNOUNCE_LONE_MS;
    if (now - lastAnnounce >= announceInterval) {
        lastAnnounce = now;
        burstRemaining = MESH_BURST_LEN - 1;
        burstNextAt = now;
    }

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
