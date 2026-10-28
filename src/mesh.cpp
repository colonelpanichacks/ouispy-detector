#include "mesh.h"
#include "mesh_ota.h"
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Preferences.h>
#include <rom/crc.h>
#include <string.h>
#include <esp_mac.h>
#include <mbedtls/md.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include <esp_random.h>

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
// Announce body:  {u8 nodeId[3], u8 role, u8 channel}  (channel: the
//                  sender's mesh channel; older builds send no channel byte)
// Detection body: {u8 nodeId[3], i8 rssi, u8 dtype, u8 flags(bit0=wifi),
//                  char mac[18], char ident[18], char desc[40], u16 detId}
//                  detId (origin's detection counter) makes delivery
//                  reliable: the home acks it, the origin retries until
//                  acked, every node alerts once per (origin, detId).
// Det-ack body:   {u8 nodeId[3] (home), u8 origin[3], u16 detId}
// Command body:   {u8 nodeId[3], u8 cmd, u16 cmdId, u16 offset, u8 more,
//                  u32 crc32, data[]}  (crc covers the reassembled payload)
// Ack body:       {u8 nodeId[3], u8 cmd, u16 cmdId, u8 status, u8 count}
// Bye body:       {u8 nodeId[3], u8 reason}
// Status body:    {u8 nodeId[3], data[]}  (management reply, layout owned
//                  by main.cpp; flooded, ttl MESH_TTL)
//
// Encryption (fleet key set): EVERY frame is sealed whole — nothing of the
// layout above is visible on air:
//   frame     = nonce[12] | AES-256-GCM(plain) | tag[8]
//   plain     = len | u16 boot | u32 ctr | packet[len] | zero padding
//   frame len = 64, 128, 192 or 250 bytes (padded: announces, acks and
//               detections look alike)
// key   = SHA-256("OUISPY-MESH-ENC-v1" | fleet key); nonce = 12 random bytes
//         from the hardware RNG, fresh for every transmission (relays
//         re-seal, so a relayed copy shares no bytes with the original).
// boot/ctr belong to the frame's ORIGIN (boot counts up every boot in NVS)
// and feed a per-sender replay window. No magic, type, seq or node ID in
// the clear: a receiver recognises fleet frames by authenticating them.
#define MESH_MAGIC0   'O'
#define MESH_MAGIC1   'S'
#define MESH_VERSION  3
#define MESH_T_ANNOUNCE 1
#define MESH_T_DETECT   2
#define MESH_T_COMMAND  3
#define MESH_T_ACK      4
#define MESH_T_BYE      5
#define MESH_T_DETACK   7    // 6 is the esp-lr-mesh library's DATA_TO
#define MESH_T_STATUS   8
#define MESH_T_OTA      9    // firmware update session (mesh_ota.cpp)
#define MESH_T_NACK     10   // chunks a command transfer is missing (receiver -> sender)
#define MESH_SEAL_NONCE 12
#define MESH_SEAL_TAG   8
#define MESH_SEAL_INNER 7      // len + u16 boot + u32 ctr ahead of the packet
static const uint8_t kSealSizes[] = { 64, 128, 192, 250 };   // padded frame sizes
#define MESH_TTL        5    // hop budget: origin + up to 4 relays
#define MESH_HDR_LEN    12
#define MESH_MIN_LEN    (MESH_HDR_LEN + 3)   // header + nodeId: every packet has both
#define MESH_PKT_MAX    250
#define MESH_BODY       MESH_HDR_LEN
#define MESH_DETECT_LEN (MESH_HDR_LEN + 3 + 1 + 1 + 1 + 18 + 18 + 40)   // legacy, no detId
#define MESH_DETECT_ID_LEN (MESH_DETECT_LEN + 2)
#define MESH_DETACK_LEN (MESH_HDR_LEN + 3 + 3 + 2)
#define MESH_CMD_MIN    (MESH_HDR_LEN + 3 + 1 + 2 + 2 + 1 + 4)
#define MESH_ACK_LEN    (MESH_HDR_LEN + 3 + 1 + 2 + 1 + 1)
#define MESH_BYE_LEN    (MESH_HDR_LEN + 3 + 1)
#define MESH_NACK_LEN   (MESH_HDR_LEN + 3 + 3 + 2 + 4)   // requester, sender, cmdId, missing mask
#define MESH_NACK_QUIET_MS 1200   // transfer silent this long with holes: ask for them
#define MESH_NACK_TRIES    6
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
#define MESH_RXQ_SIZE   16   // retries + relays arrive in bursts
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

// ================================
// Reliable detection delivery
// ================================
// The origin keeps each detection here and resends it (fresh seq, so relays
// forward it again) until a home node acks it or MESH_DET_GIVEUP_MS passes.
// The sweep visits the mesh channel every ~300 ms (main.cpp), so early
// retries are tight; gaps grow and carry jitter so they never lock onto a
// peer's cycle. With no home in the roster (or when this node IS the home)
// nobody acks, so a detection goes out MESH_DET_BLIND times instead,
// spanning more than a peer's longest off-channel stretch.
#define MESH_DET_PENDING   12
#define MESH_DET_GIVEUP_MS 30000UL
#define MESH_DET_BLIND     4
#define MESH_DET_SEEN      32
#define MESH_DET_SEEN_TTL_MS 120000UL
static const uint16_t kDetRetryMs[] = { 90, 90, 100, 110, 130, 160, 200, 300, 500, 800, 1200, 2000, 3000 };
struct MeshPendDet { bool used; uint16_t detId; uint8_t body[MESH_DETECT_ID_LEN - MESH_HDR_LEN];
                     uint32_t firstAt, nextAt; uint8_t tries; };
static MeshPendDet pendDet[MESH_DET_PENDING];
static uint16_t detCounter = 0;
struct MeshDetSeen { uint8_t origin[3]; uint16_t detId; uint32_t ts; bool live; };
static MeshDetSeen detSeen[MESH_DET_SEEN];
static uint32_t detSent = 0, detRetries = 0, detAcked = 0, detExpired = 0, detEvicted = 0;

static bool meshStarted = false;
static uint16_t encBoot = 0;   // encryption nonce epoch (NVS, +1 per boot)
static uint32_t encCtr = 0;    // frames sealed this boot
static void encSetup();
static bool meshLrRateOk = false;   // ESP-NOW really transmitting at the LR rate
static volatile bool txInflight = false;   // a frame is on the air, awaiting its send callback
static bool meshParked = false;
static uint8_t selfMac[6] = {0};
static uint8_t selfId[3] = {0};   // factory MAC tail — the stable node ID
static uint8_t meshSeq = 0;

// (origin, seq) duplicate cache — drops re-flooded copies within the TTL
// window. Our own packets never need an entry: the receive path drops
// anything carrying our origin MAC before the cache is consulted.
struct MeshSeen { uint8_t origin[6]; uint8_t seq; uint32_t ts; bool live; volatile uint8_t copies; };
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
static uint8_t rxChan[MESH_RXQ_SIZE];   // radio channel when each frame arrived
static uint8_t rxFrom[MESH_RXQ_SIZE][6];  // transmitter: the neighbor (origin or relay) we heard

// The channel the radio is on right now (0 = unknown, e.g. an MQTT STA
// window). Frames heard on any OTHER channel than the mesh's come from a
// node parked somewhere else, caught while our sweep passed its channel:
// they must not count as a link. Treating them as one hid split fleets —
// the roster flapped, and heal never ran because the stray frames kept
// resetting its silence timer.
static volatile uint8_t radioCh = 0;

// Stray sightings: nodes heard on a channel other than ours.
#define MESH_STRAY_SLOTS 4
struct MeshStray { uint8_t id[3]; uint8_t ch; uint8_t role; uint32_t ts; };
static MeshStray strays[MESH_STRAY_SLOTS];

// Remote fast-follow: a remote that hears the HOME announcing on another
// channel moves there (the home is the fleet's channel anchor and never
// heals on its own). Needs repeated sightings so one odd frame can't move it.
#define MESH_FOLLOW_HITS     3
#define MESH_FOLLOW_WINDOW_MS 15000UL
static uint8_t  followCh = 0, followHits = 0;
static uint32_t followFirstAt = 0, homeLinkedAt = 0;
static uint8_t rxHead = 0, rxTail = 0, rxCount = 0;
static portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

// Roster of other nodes, keyed by the STABLE nodeId (never the MAC).
struct MeshNode { uint8_t id[3]; uint32_t lastSeen; uint8_t role; uint32_t pkts; bool live; uint8_t mac[6];
                 uint32_t lastDirect; };   // last frame straight from it (not relayed): a link
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
// Selective repeat: a receiver missing chunks after the rounds names them in
// a NACK, and the sender (which still holds outBuf) sends just those.
// Broadcast frames have no link-layer retry and a burst loses random chunks
// (BLE coexistence, a busy loop pass), so rounds alone often never filled
// every gap of a big transfer (a 33-filter report is 8 chunks).
static uint32_t outResendMask = 0;
static uint32_t outDoneAt = 0;          // last round finished (NACKs honored for 30 s)

// Command repeat rounds. Every node time-slices its radio (WiFi sweep, BLE
// scan, mesh window), so a command sent once only lands if the receiver is
// on the mesh channel at that moment — in scanning mode most single-shot
// commands simply vanished. A transfer now goes out in several rounds
// spread over ~4 s (longer than a receiver's sweep cycle); receivers drop
// rounds of a command they already applied (doneRecently, plus an RTC copy
// that survives the reboot a REBOOT/FILTERS/PRESET command causes). Big
// transfers repeat less so they don't hog the channel.
static const uint16_t kCmdRoundGapMs[] = { 150, 250, 350, 450, 600, 800, 1000 };
static uint8_t  outRepeatLeft = 0, outRoundIdx = 0;
static uint32_t outRepeatAt = 0;

// Listen window: after sending a command (or while owing a reply) the node
// keeps its radio on the mesh channel, without BLE, so the answers coming
// back aren't lost to a BLE scan (coexistence gives it ~2/3 of the air).
static uint32_t listenUntil = 0;

// Acks go out three times (receivers would otherwise miss them the same
// way); the commander counts and reports each node's ack once.
#define MESH_ACK_COPIES 3
struct MeshAckRep { bool used; uint8_t body[5]; uint8_t left; uint32_t nextAt; };
static MeshAckRep ackRep[4];
struct MeshAckSeen { uint8_t node[3]; uint16_t cmdId; uint32_t ts; };
static MeshAckSeen ackSeen[16];

// Fleet key replay protection: highest counter accepted per commander, and
// this node's own send counter. Both persist across reboots.
struct MeshKeyCtr { uint8_t id[3]; uint32_t ctr; };
static MeshKeyCtr keyCtr[MESH_KEYCTR_SLOTS];
static uint32_t keyTxCtr = 0;

// RSSI sniffing: the promiscuous callback (sweep phases) sees every frame
// with rx_ctrl, including our own ESP-NOW broadcasts — signal strength is
// recorded here keyed by nodeId for range diagnostics and the UI JSON.
#define MESH_SNIFF_SLOTS 8
static uint8_t  sniffMac[MESH_SNIFF_SLOTS][6];   // origin MAC (clear header; the node ID may be encrypted)
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

// True while this node has something time-critical to put on the air: a
// detection younger than MESH_URGENT_MS still awaiting delivery, queued
// frames, or a command transfer. The sweep holds the mesh channel while
// it is set, so the frame meets the receiver's next mesh visit instead of
// waiting for this node's own schedule to come round.
#define MESH_URGENT_MS 1500UL
static bool meshRelaysPending();
bool meshUrgent() {
    if (!meshStarted) return false;
    if (txCount > 0 || outActive || outRepeatLeft || outResendMask || meshRelaysPending()) return true;
    if ((int32_t)(listenUntil - millis()) > 0) return true;
    uint32_t now = millis();
    for (int i = 0; i < MESH_DET_PENDING; i++) {
        if (pendDet[i].used && now - pendDet[i].firstAt < MESH_URGENT_MS) return true;
    }
    return false;
}

void meshDeliveryStats(uint32_t& sent, uint32_t& retries, uint32_t& acked,
                       uint32_t& expired, uint8_t& pending) {
    sent = detSent; retries = detRetries; acked = detAcked;
    expired = detExpired + detEvicted;
    pending = 0;
    for (int i = 0; i < MESH_DET_PENDING; i++) if (pendDet[i].used) pending++;
}

void meshSniffRssi(const uint8_t* mac, int8_t rssi) {
    static const uint8_t zero[6] = {0};
    uint32_t now = millis();
    int slot = -1, empty = -1;
    for (int i = 0; i < MESH_SNIFF_SLOTS; i++) {
        if (memcmp(sniffMac[i], mac, 6) == 0) { slot = i; break; }
        if (empty < 0 && memcmp(sniffMac[i], zero, 6) == 0) empty = i;
    }
    if (slot < 0) slot = empty;
    if (slot < 0) {
        uint32_t oldest = 0xFFFFFFFF;
        for (int i = 0; i < MESH_SNIFF_SLOTS; i++) {
            if (sniffTs[i] < oldest) { oldest = sniffTs[i]; slot = i; }
        }
    }
    memcpy(sniffMac[slot], mac, 6);
    sniffRssi[slot] = rssi;
    sniffTs[slot] = now;
}

int8_t meshLinkRssi() {
    uint32_t now = millis();
    int8_t best = 0;
    uint32_t bestTs = 0;
    for (int i = 0; i < MESH_SNIFF_SLOTS; i++) {
        if (!sniffTs[i]) continue;
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
    // Encryption nonce epoch: counts up every boot (and is saved first), so
    // (boot, frame counter) never repeats even though the counter restarts.
    {
        Preferences w;
        w.begin("ouispy", false);
        encBoot = (uint16_t)(w.getUShort("mk_boot", (uint16_t)(esp_random() & 0x7FFF)) + 1);
        w.putUShort("mk_boot", encBoot);
        w.end();
        encCtr = 0;
    }
    encSetup();
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
    encSetup();   // traffic is (re)keyed immediately
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
        if (seenCache[i].seq == seq && memcmp(seenCache[i].origin, origin, 6) == 0) {
            if (seenCache[i].copies < 255) seenCache[i].copies++;   // another node relayed it
            return true;
        }
        if (age >= oldestAge) { oldestAge = age; oldest = i; }
    }
    int slot = (freeSlot >= 0) ? freeSlot : oldest;
    if (slot < 0) return false;
    memcpy(seenCache[slot].origin, origin, 6);
    seenCache[slot].seq = seq;
    seenCache[slot].ts = now;
    seenCache[slot].copies = 0;
    seenCache[slot].live = true;
    return false;
}

// How many other copies of a frame have been heard (0 = only the first).
static uint8_t meshSeenCopies(const uint8_t* origin, uint8_t seq) {
    for (int i = 0; i < MESH_SEEN_SIZE; i++) {
        if (seenCache[i].live && seenCache[i].seq == seq &&
            memcmp(seenCache[i].origin, origin, 6) == 0) return seenCache[i].copies;
    }
    return 0;
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

// Nodes heard first-hand in the last 20 s: {id[3], rssi, age s} each.
// The dashboard draws the mesh's real links from every node's list.
uint8_t meshNeighbors(uint8_t* out, uint8_t max) {
    uint32_t now = millis();
    uint8_t n = 0;
    for (int i = 0; i < MESH_ROSTER_SIZE && n < max; i++) {
        const MeshNode& r = roster[i];
        if (!r.live || !r.lastDirect || now - r.lastDirect > 20000) continue;   // gone quiet: link down
        int8_t rssi = 0;
        for (int s = 0; s < MESH_SNIFF_SLOTS; s++) {
            if (sniffTs[s] && memcmp(sniffMac[s], r.mac, 6) == 0 && (int32_t)(now - sniffTs[s]) < 60000) { rssi = sniffRssi[s]; break; }
        }
        uint32_t age = (now - r.lastDirect) / 1000;
        uint8_t* o = out + n * 5;
        memcpy(o, r.id, 3);
        o[3] = (uint8_t)rssi;
        o[4] = (uint8_t)(age > 255 ? 255 : age);
        n++;
    }
    return n;
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
            if (memcmp(sniffMac[s], n.mac, 6) == 0 && now - sniffTs[s] < 10000) {
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
// ================================
// Encryption (fleet key)
// ================================
static mbedtls_gcm_context encGcm;
static bool encReady = false;
static uint32_t encDropPlain = 0, encDropAuth = 0, encDropReplay = 0;
// Replay window per sender: highest (boot, ctr) seen + a 32-frame bitmap
// for frames that arrive slightly out of order (relays, repeat copies).
struct MeshReplay { uint8_t id[3]; uint16_t boot; uint32_t top; uint32_t bits; uint32_t ts; };
static MeshReplay replayWin[12];

static void encSetup() {
    if (encReady) { mbedtls_gcm_free(&encGcm); encReady = false; }
    memset(replayWin, 0, sizeof(replayWin));
    if (!meshCfg.key[0]) return;
    uint8_t k[32];
    mbedtls_sha256_context sh;
    mbedtls_sha256_init(&sh);
    mbedtls_sha256_starts(&sh, 0);
    mbedtls_sha256_update(&sh, (const unsigned char*)"OUISPY-MESH-ENC-v1", 18);
    mbedtls_sha256_update(&sh, (const unsigned char*)meshCfg.key, strlen(meshCfg.key));
    mbedtls_sha256_finish(&sh, k);
    mbedtls_sha256_free(&sh);
    mbedtls_gcm_init(&encGcm);
    encReady = mbedtls_gcm_setkey(&encGcm, MBEDTLS_CIPHER_ID_AES, k, 256) == 0;
    memset(k, 0, sizeof(k));
}

// Relay context: a relayed frame keeps its ORIGIN's boot/ctr (the replay
// windows downstream key on the origin), only the nonce is fresh.
static bool sealRelay = false;
static uint16_t curBoot = 0;
static uint32_t curCtr = 0;

// Seal packet[len] into out (a whole frame). Returns the frame length, 0 if
// the packet is too big to seal.
static uint8_t encSeal(const uint8_t* packet, uint8_t len, uint8_t* out) {
    uint8_t inner = 0;
    for (uint8_t sz : kSealSizes) {
        if (len + MESH_SEAL_INNER + MESH_SEAL_NONCE + MESH_SEAL_TAG <= sz) { inner = sz - MESH_SEAL_NONCE - MESH_SEAL_TAG; break; }
    }
    if (!inner) return 0;
    uint8_t plain[MESH_PKT_MAX];
    memset(plain, 0, inner);
    uint16_t boot = sealRelay ? curBoot : encBoot;
    uint32_t ctr  = sealRelay ? curCtr  : ++encCtr;
    plain[0] = len;
    memcpy(plain + 1, &boot, 2);
    memcpy(plain + 3, &ctr, 4);
    memcpy(plain + MESH_SEAL_INNER, packet, len);
    esp_fill_random(out, MESH_SEAL_NONCE);
    if (mbedtls_gcm_crypt_and_tag(&encGcm, MBEDTLS_GCM_ENCRYPT, inner, out, MESH_SEAL_NONCE,
                                  nullptr, 0, plain, out + MESH_SEAL_NONCE,
                                  MESH_SEAL_TAG, out + MESH_SEAL_NONCE + inner) != 0) return 0;
    return inner + MESH_SEAL_NONCE + MESH_SEAL_TAG;
}

// Sliding-window replay check for (sender, boot, ctr); records it if fresh.
static bool replayFresh(const uint8_t* id, uint16_t boot, uint32_t ctr, uint32_t now) {
    int slot = -1, oldest = 0;
    for (int i = 0; i < 12; i++) {
        if (replayWin[i].ts && memcmp(replayWin[i].id, id, 3) == 0) { slot = i; break; }
        if (replayWin[i].ts < replayWin[oldest].ts) oldest = i;
    }
    if (slot < 0) {   // first frame from this sender since we booted / rekeyed
        slot = oldest;
        memcpy(replayWin[slot].id, id, 3);
        replayWin[slot].boot = boot;
        replayWin[slot].top = ctr;
        replayWin[slot].bits = 1;
        replayWin[slot].ts = now ? now : 1;
        return true;
    }
    MeshReplay& w = replayWin[slot];
    w.ts = now ? now : 1;
    if ((int16_t)(boot - w.boot) < 0) return false;          // an older boot session
    if (boot != w.boot) { w.boot = boot; w.top = ctr; w.bits = 1; return true; }   // it rebooted
    if (ctr > w.top) {
        uint32_t shift = ctr - w.top;
        w.bits = (shift >= 32) ? 1 : ((w.bits << shift) | 1);
        w.top = ctr;
        return true;
    }
    uint32_t back = w.top - ctr;
    if (back >= 32 || (w.bits & (1UL << back))) return false;
    w.bits |= (1UL << back);
    return true;
}

// frame -> packet (into out). Fails on a wrong key / tampering, a malformed
// inside or a replay. boot/ctr of the origin are returned for relaying.
static bool encOpen(const uint8_t* frame, uint8_t flen, uint8_t* out, uint8_t& len,
                    uint16_t& boot, uint32_t& ctr, uint32_t now) {
    if (flen < MESH_SEAL_NONCE + MESH_SEAL_TAG + MESH_SEAL_INNER + MESH_MIN_LEN) return false;
    uint8_t inner = flen - MESH_SEAL_NONCE - MESH_SEAL_TAG;
    uint8_t plain[MESH_PKT_MAX];
    if (mbedtls_gcm_auth_decrypt(&encGcm, inner, frame, MESH_SEAL_NONCE, nullptr, 0,
                                 frame + MESH_SEAL_NONCE + inner, MESH_SEAL_TAG,
                                 frame + MESH_SEAL_NONCE, plain) != 0) {
        encDropAuth++;   // not ours: another fleet's key, another ESP-NOW device, or tampered
        return false;
    }
    len = plain[0];
    if (len < MESH_MIN_LEN || len > inner - MESH_SEAL_INNER) return false;
    memcpy(&boot, plain + 1, 2);
    memcpy(&ctr, plain + 3, 4);
    memcpy(out, plain + MESH_SEAL_INNER, len);
    if (out[0] != MESH_MAGIC0 || out[1] != MESH_MAGIC1 || out[2] != MESH_VERSION) return false;
    return true;
}

void meshEncStats(bool& on, uint32_t& dropPlain, uint32_t& dropAuth, uint32_t& dropReplay) {
    on = encReady;
    dropPlain = encDropPlain;
    dropAuth = encDropAuth;
    dropReplay = encDropReplay;
}

// Every frame goes through here: with a fleet key it is sealed on the way
// into the queue (relays included — they re-seal with a fresh nonce).
static bool meshTxPush(const uint8_t* pkt, uint8_t len) {
    if (txCount >= MESH_TXQ_SIZE) return false;
    if (encReady) {
        len = encSeal(pkt, len, txQ[txHead]);
        if (!len) return false;
    } else {
        memcpy(txQ[txHead], pkt, len);
    }
    txLen[txHead] = len;
    txHead = (txHead + 1) % MESH_TXQ_SIZE;
    txCount++;
    return true;
}

// Feed the next chunks of the outgoing command into the TX queue, leaving
// MESH_TXQ_RESERVE slots for announces, acks and relays. Chunk k carries
// bytes [180k, 180k+180); the final chunk carries the CRC of the payload.
static uint16_t meshPushChunk(uint16_t off);
static void meshPumpCommand() {
    if (!outActive && outRepeatLeft && (int32_t)(millis() - outRepeatAt) >= 0) {
        outOff = 0;            // next round: the whole transfer again
        outActive = true;
        outRepeatLeft--;
    }
    while (outActive && txCount < MESH_TXQ_SIZE - MESH_TXQ_RESERVE) {
        uint16_t chunkLen = meshPushChunk(outOff);
        bool more = (outOff + chunkLen) < outLen;
        outOff += chunkLen;
        if (!more) {
            outActive = false;
            if (outRepeatLeft) {
                outRepeatAt = millis() + kCmdRoundGapMs[min((int)outRoundIdx, 6)] + random(0, 80);
                outRoundIdx++;
            } else {
                outDoneAt = millis();
            }
        }
    }
    // Rounds done: resend only the chunks a receiver said it is missing.
    while (!outActive && !outRepeatLeft && outResendMask &&
           txCount < MESH_TXQ_SIZE - MESH_TXQ_RESERVE) {
        uint8_t k = (uint8_t)__builtin_ctz(outResendMask);
        outResendMask &= ~(1UL << k);
        uint32_t off = (uint32_t)k * MESH_CMD_CHUNK;
        if (off < outLen) meshPushChunk((uint16_t)off);
    }
}

// Chunk at `off` of the outgoing transfer into the TX queue; returns its length.
static uint16_t meshPushChunk(uint16_t off) {
    uint16_t chunkLen = min((uint16_t)MESH_CMD_CHUNK, (uint16_t)(outLen - off));
    bool more = (off + chunkLen) < outLen;
    uint8_t pkt[MESH_PKT_MAX];
    uint8_t n = meshBuildHeader(pkt, MESH_T_COMMAND, MESH_TTL);
    n += meshBuildBodyId(pkt + n);
    pkt[n++] = outCmd;
    pkt[n++] = (uint8_t)(outCmdId & 0xFF);
    pkt[n++] = (uint8_t)(outCmdId >> 8);
    pkt[n++] = (uint8_t)(off & 0xFF);
    pkt[n++] = (uint8_t)(off >> 8);
    pkt[n++] = more ? 1 : 0;
    uint32_t crc = more ? 0 : outCrc;
    memcpy(pkt + n, &crc, 4); n += 4;
    if (chunkLen > 0) { memcpy(pkt + n, outBuf + off, chunkLen); n += chunkLen; }
    meshTxPush(pkt, n);
    return chunkLen;
}

// Receiver side: ask `sender` for the chunks of transfer `cmdId` in `mask`.
static void meshSendNack(const uint8_t* sender, uint16_t cmdId, uint32_t mask) {
    uint8_t pkt[MESH_NACK_LEN];
    uint8_t n = meshBuildHeader(pkt, MESH_T_NACK, MESH_TTL);
    n += meshBuildBodyId(pkt + n);
    memcpy(pkt + n, sender, 3); n += 3;
    pkt[n++] = (uint8_t)(cmdId & 0xFF);
    pkt[n++] = (uint8_t)(cmdId >> 8);
    memcpy(pkt + n, &mask, 4); n += 4;
    meshTxPush(pkt, n);
}

// Sender side: a NACK for our last transfer.
static void meshOnNack(const uint8_t* body) {
    uint16_t id = body[6] | (body[7] << 8);
    uint32_t mask;
    memcpy(&mask, body + 8, 4);
    if (id != outCmdId || outLen == 0 || outActive || outRepeatLeft ||
        (int32_t)(millis() - outDoneAt) > 30000) return;
    uint16_t chunks = (outLen + MESH_CMD_CHUNK - 1) / MESH_CMD_CHUNK;
    if (chunks < 32) mask &= (1UL << chunks) - 1;
    if (!mask) return;
    outResendMask |= mask;
    meshHoldRadio(3000);
    if (isSerialConnected()) {
        Serial.printf("[MESH] resending chunks %08lX of transfer %u on request\n", (unsigned long)mask, id);
    }
}

// Flush the TX queue — ONLY while the radio is parked on the mesh channel.
// Sending while a WiFi sweep owns the radio (or mid channel hop) fails at
// the driver level. Packets wait for the next BLE phase; the loop calls
// this every pass, so the delay is at most one phase (~2.2 s). A frame the
// driver rejects (typically a BLE coexistence collision) gets exactly one
// retry next flush instead of vanishing.
static void meshPumpDetections(uint32_t now);
static void meshAckRepeatTick(uint32_t now);
static void meshRelayTick(uint32_t now);

void meshFlushTxNow() {
    if (!meshStarted || !meshParked) return;
    meshPumpCommand();
    meshAckRepeatTick(millis());   // here, not in meshTick: pre-reboot flush loops only call this
    meshPumpDetections(millis());
    meshRelayTick(millis());
    uint8_t bcast[6];
    memset(bcast, 0xFF, 6);
    // One frame on the air at a time: the next goes out after the driver's
    // send-done callback for the last one (25 ms cap). Firing the whole
    // queue back-to-back overflowed ESP-NOW's internal buffer — at the LR
    // rate a frame is ~4x longer on air — and the driver rejected frames,
    // sometimes the very command being delivered. A rejected frame stays at
    // the head and is retried (3 tries) instead of being thrown away.
    static uint8_t headTries = 0;
    while (txCount > 0) {
        uint32_t t0 = millis();
        while (txInflight && millis() - t0 < 25) delay(1);
        txInflight = false;
        if (esp_now_send(bcast, txQ[txTail], txLen[txTail]) == ESP_OK) {
            meshTxSent++;
            txInflight = true;
            headTries = 0;
        } else {
            meshTxFail++;
            if (++headTries < 3) { delay(2); continue; }
            headTries = 0;   // give up on this one, keep the queue moving
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
    left += outRepeatLeft;                  // rounds still to come
    for (int i = 0; i < 4; i++) {           // ack copies still to come: a node
        if (ackRep[i].used) left += ackRep[i].left;   // about to reboot waits for them
    }
    uint16_t total = txCount + left;
    return total > 255 ? 255 : (uint8_t)total;
}

void meshSelfIdHex(char out[7]) {
    uint8_t baseMac[6];
    esp_efuse_mac_get_default(baseMac);
    snprintf(out, 7, "%02X%02X%02X", baseMac[3], baseMac[4], baseMac[5]);
}

// ---- for mesh_ota.cpp ----
void meshSelfIdBytes(uint8_t out[3]) { memcpy(out, selfId, 3); }
bool meshSealed() { return encReady; }
uint8_t meshTxFree() { return (uint8_t)(MESH_TXQ_SIZE - txCount); }
// One OTA frame: header + body. ttl 1 = direct only, MESH_TTL = may relay.
bool meshOtaSend(const uint8_t* body, uint8_t len, bool relay) {
    if (!meshStarted || txCount >= MESH_TXQ_SIZE || MESH_HDR_LEN + len > MESH_PKT_MAX) return false;
    uint8_t pkt[MESH_PKT_MAX];
    uint8_t n = meshBuildHeader(pkt, MESH_T_OTA, relay ? MESH_TTL : 1);
    memcpy(pkt + n, body, len);
    return meshTxPush(pkt, n + len);
}
bool meshHomeHeard() {
    uint32_t now = millis();
    for (int i = 0; i < MESH_ROSTER_SIZE; i++) {
        if (roster[i].live && roster[i].role == 1 && now - roster[i].lastSeen < MESH_ROSTER_TIMEOUT) return true;
    }
    return false;
}

static bool meshHomeKnown(uint32_t now) {
    for (int i = 0; i < MESH_ROSTER_SIZE; i++) {
        if (roster[i].live && roster[i].role == 1 && now - roster[i].lastSeen < MESH_ROSTER_TIMEOUT) return true;
    }
    return false;
}

void meshQueueDetection(const char* mac, const char* ident, const char* desc,
                        int rssi, uint8_t dtype, bool wifiDomain, bool update) {
    if (!meshStarted) return;
    uint32_t now = millis();
    // Claim a pending slot; when every slot is busy, evict the entry that
    // has been trying longest — it has had the most chances already.
    int slot = -1;
    uint32_t oldestAge = 0;
    for (int i = 0; i < MESH_DET_PENDING; i++) {
        if (!pendDet[i].used) { slot = i; break; }
        uint32_t age = now - pendDet[i].firstAt;
        if (slot < 0 || age > oldestAge) { oldestAge = age; slot = i; }
    }
    if (pendDet[slot].used) {
        detEvicted++;
        if (isSerialConnected()) Serial.println("[MESH] delivery table full, oldest detection given up");
    }
    MeshPendDet& d = pendDet[slot];
    uint8_t* b = d.body;
    memcpy(b, selfId, 3);
    b[3] = (uint8_t)(int8_t)rssi;
    b[4] = dtype;
    b[5] = (wifiDomain ? 0x01 : 0x00) | (update ? 0x02 : 0x00);   // older homes ignore bit1
    memset(b + 6, 0, 18 + 18 + 40);
    memcpy(b + 6,  mac,   min(strlen(mac) + 1, (size_t)18));
    memcpy(b + 24, ident, min(strlen(ident) + 1, (size_t)18));
    memcpy(b + 42, desc,  min(strlen(desc) + 1, (size_t)40));
    d.detId = ++detCounter;
    if (d.detId == 0) d.detId = ++detCounter;
    b[82] = (uint8_t)(d.detId & 0xFF);
    b[83] = (uint8_t)(d.detId >> 8);
    d.firstAt = now;
    d.nextAt = now;
    d.tries = 0;
    d.used = true;
}

// Push due detections into the TX queue (fresh header each time), keeping
// MESH_TXQ_RESERVE slots free for announces, acks and relays.
static void meshPumpDetections(uint32_t now) {
    bool homeKnown = meshHomeKnown(now);
    for (int i = 0; i < MESH_DET_PENDING; i++) {
        MeshPendDet& d = pendDet[i];
        if (!d.used || (int32_t)(now - d.nextAt) < 0) continue;
        bool waitAck = homeKnown && !meshCfg.home;
        if ((!waitAck && d.tries >= MESH_DET_BLIND) || now - d.firstAt > MESH_DET_GIVEUP_MS) {
            if (waitAck) {
                detExpired++;
                if (isSerialConnected()) Serial.printf("[MESH] detection %u not acked by home in %lus, given up\n",
                                                       d.detId, MESH_DET_GIVEUP_MS / 1000);
            }
            d.used = false;
            continue;
        }
        if (txCount >= MESH_TXQ_SIZE - MESH_TXQ_RESERVE) return;
        uint8_t pkt[MESH_PKT_MAX];
        uint8_t n = meshBuildHeader(pkt, MESH_T_DETECT, MESH_TTL);
        memcpy(pkt + n, d.body, sizeof(d.body));
        n += sizeof(d.body);
        meshTxPush(pkt, n);
        if (d.tries == 0) detSent++; else detRetries++;
        uint8_t k = d.tries < (sizeof(kDetRetryMs) / sizeof(kDetRetryMs[0]))
                        ? d.tries : (sizeof(kDetRetryMs) / sizeof(kDetRetryMs[0])) - 1;
        d.tries++;
        d.nextAt = now + kDetRetryMs[k] + (uint32_t)random(0, 50);
    }
}

// Home side: alert once per (origin, detId); remember it for re-acks.
// Returns true the first time a detection is seen.
static bool detSeenFirst(const uint8_t* origin, uint16_t detId, uint32_t now) {
    int freeSlot = -1, oldest = -1;
    uint32_t oldestAge = 0;
    for (int i = 0; i < MESH_DET_SEEN; i++) {
        if (!detSeen[i].live) { if (freeSlot < 0) freeSlot = i; continue; }
        uint32_t age = now - detSeen[i].ts;
        if (age >= MESH_DET_SEEN_TTL_MS) { if (freeSlot < 0) freeSlot = i; continue; }
        if (detSeen[i].detId == detId && memcmp(detSeen[i].origin, origin, 3) == 0) return false;
        if (age >= oldestAge) { oldestAge = age; oldest = i; }
    }
    int slot = (freeSlot >= 0) ? freeSlot : oldest;
    if (slot < 0) return true;
    memcpy(detSeen[slot].origin, origin, 3);
    detSeen[slot].detId = detId;
    detSeen[slot].ts = now;
    detSeen[slot].live = true;
    return true;
}

static void meshSendDetAck(const uint8_t* origin, uint16_t detId) {
    uint8_t pkt[MESH_DETACK_LEN];
    uint8_t n = meshBuildHeader(pkt, MESH_T_DETACK, MESH_TTL);
    n += meshBuildBodyId(pkt + n);
    memcpy(pkt + n, origin, 3); n += 3;
    pkt[n++] = (uint8_t)(detId & 0xFF);
    pkt[n++] = (uint8_t)(detId >> 8);
    meshTxPush(pkt, n);   // full queue: the origin simply retries
}

static void meshSendAnnounce() {
    uint8_t pkt[MESH_BYE_LEN + 2];
    uint8_t n = meshBuildHeader(pkt, MESH_T_ANNOUNCE, 1);
    n += meshBuildBodyId(pkt + n);
    pkt[n++] = meshCfg.home ? 1 : 0;
    // Our channel, stated outright. The radio can't be trusted to tell:
    // 2.4 GHz channels overlap, so up close a frame sent on channel 4 is
    // also decoded by a node parked on 6 and looks like a working link —
    // until the nodes move apart and it vanishes.
    pkt[n++] = meshCfg.channel;
    // Announces repeat every 1.5 s, so when the queue is nearly full (a
    // command's repeat rounds, acks, status replies) skip this one quietly
    // and leave the slots for traffic that matters.
    if (txCount >= MESH_TXQ_SIZE - 1) return;
    meshTxPush(pkt, n);
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
    if (!meshStarted || outActive || outRepeatLeft) return false;
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
    uint16_t chunks = (len + MESH_CMD_CHUNK - 1) / MESH_CMD_CHUNK;
    // 8 / 4 / 3 rounds. The receiver keeps every chunk it got across rounds,
    // so a big transfer (a 30+ filter report is ~10 chunks) needs a third
    // pass less to fill gaps than to survive a weak link at all.
    outRepeatLeft = (chunks <= 2) ? 7 : (chunks <= 6) ? 3 : 2;
    outRoundIdx = 0;
    outResendMask = 0;
    meshHoldRadio(8000);   // rounds take ~4 s; replies trail them by a few more
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
    // Schedule the extra copies (fresh seq each, so relays forward them).
    int slot = 0;
    for (int i = 0; i < 4; i++) {
        if (!ackRep[i].used) { slot = i; break; }
        if (ackRep[i].nextAt < ackRep[slot].nextAt) slot = i;
    }
    memcpy(ackRep[slot].body, pkt + MESH_HDR_LEN + 3, 5);
    ackRep[slot].left = MESH_ACK_COPIES - 1;
    ackRep[slot].nextAt = millis() + 250 + random(0, 150);
    ackRep[slot].used = true;
}

static void meshAckRepeatTick(uint32_t now) {
    for (int i = 0; i < 4; i++) {
        MeshAckRep& a = ackRep[i];
        if (!a.used || (int32_t)(now - a.nextAt) < 0 || txCount >= MESH_TXQ_SIZE) continue;
        uint8_t pkt[MESH_ACK_LEN];
        uint8_t n = meshBuildHeader(pkt, MESH_T_ACK, MESH_TTL);
        n += meshBuildBodyId(pkt + n);
        memcpy(pkt + n, a.body, 5);
        n += 5;
        meshTxPush(pkt, n);
        if (--a.left == 0) a.used = false;
        else a.nextAt = now + 500 + random(0, 300);
    }
}

// True the first time (node, cmdId) acks within 30 s; copies return false.
static bool ackFirst(const uint8_t* node, uint16_t cmdId, uint32_t now) {
    int slot = 0;
    for (int i = 0; i < 16; i++) {
        if (ackSeen[i].ts && now - ackSeen[i].ts < 30000 && ackSeen[i].cmdId == cmdId &&
            memcmp(ackSeen[i].node, node, 3) == 0) return false;
        if (ackSeen[i].ts < ackSeen[slot].ts) slot = i;
    }
    memcpy(ackSeen[slot].node, node, 3);
    ackSeen[slot].cmdId = cmdId;
    ackSeen[slot].ts = now ? now : 1;
    return true;
}

void meshSendStatus(const uint8_t* data, uint8_t len) {
    if (!meshStarted || len > MESH_PKT_MAX - MESH_MIN_LEN) return;
    uint8_t pkt[MESH_PKT_MAX];
    uint8_t n = meshBuildHeader(pkt, MESH_T_STATUS, MESH_TTL);
    n += meshBuildBodyId(pkt + n);
    memcpy(pkt + n, data, len);
    n += len;
    if (!meshTxPush(pkt, n) && isSerialConnected()) {
        Serial.println("[MESH] TX queue full, status dropped");
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

// The last command applied, kept in RTC RAM: it survives the software
// reset a REBOOT/FILTERS/PRESET command triggers, so the repeat rounds
// still on the air when the node comes back up aren't applied again.
struct MeshRtcDone { uint32_t magic; uint8_t origin[3]; uint16_t cmdId; };
static RTC_NOINIT_ATTR MeshRtcDone rtcDone;
#define MESH_RTC_MAGIC 0x4F53444EUL   // 'OSDN'

static bool doneRecently(const uint8_t* origin, uint16_t cmdId, uint32_t now) {
    if (rtcDone.magic == MESH_RTC_MAGIC && rtcDone.cmdId == cmdId &&
        memcmp(rtcDone.origin, origin, 3) == 0 && now < MESH_STAGE_GAP_MS) return true;
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
    memcpy(rtcDone.origin, origin, 3);
    rtcDone.cmdId = cmdId;
    rtcDone.magic = MESH_RTC_MAGIC;
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
        if (stageActive && isSerialConnected()) {
            // Diagnosable instead of silent: which chunks a lost transfer had.
            Serial.printf("[MESH] transfer %02X%02X%02X/%u (cmd %u) abandoned incomplete: chunks %08lX%s\n",
                          stageOrigin[0], stageOrigin[1], stageOrigin[2], stageCmdId, stageCmd & 0x7F,
                          (unsigned long)stageChunks, stageHaveFinal ? " (final seen)" : "");
        }
        if (more && isSerialConnected()) {
            Serial.printf("[MESH] receiving %02X%02X%02X/%u (cmd %u), first chunk @%u\n",
                          origin[0], origin[1], origin[2], cmdId, cmd & 0x7F, offset);
        }
        stageReset();
        stageActive = true;
        stageCmdId = cmdId;
        stageCmd = cmd;
        memcpy(stageOrigin, origin, 3);
    }
    stageLastChunk = now;

    if (len) memcpy(stageBuf + offset, data, len);
    stageChunks |= (1UL << (offset / MESH_CMD_CHUNK));
    // Mid-transfer: stay parked on the mesh channel for the rest of it. A
    // sweep now would miss chunks that every following round must refill.
    meshHoldRadio(2500);
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
// Minimum frame length per (plaintext) type.
static bool meshLenOk(uint8_t type, int len) {
    switch (type) {
        case MESH_T_ANNOUNCE: return len >= MESH_MIN_LEN + 1;
        case MESH_T_DETECT:   return len >= MESH_DETECT_LEN;
        case MESH_T_COMMAND:  return len >= MESH_CMD_MIN;
        case MESH_T_ACK:      return len >= MESH_ACK_LEN;
        case MESH_T_BYE:      return len >= MESH_BYE_LEN;
        case MESH_T_DETACK:   return len >= MESH_DETACK_LEN;
        case MESH_T_STATUS:   return len >= MESH_MIN_LEN + 1;
        case MESH_T_NACK:     return len >= MESH_NACK_LEN;
        case MESH_T_OTA:      return len >= MESH_MIN_LEN + 6;
        default:              return len >= MESH_MIN_LEN;
    }
}

static void meshRecvCb(const uint8_t* senderMac, const uint8_t* data, int len) {
    meshRxAll++;
    if (encReady && meshStarted) {
        // Sealed fleet: nothing to inspect in the clear. Only the padded
        // frame sizes can be ours; authentication happens in the drain.
        bool sized = false;
        for (uint8_t sz : kSealSizes) if (len == sz) sized = true;
        if (!sized) {
            if (len >= 2 && data[0] == MESH_MAGIC0 && data[1] == MESH_MAGIC1) encDropPlain++;
            else meshRxBad++;
            return;
        }
        portENTER_CRITICAL(&rxMux);
        if (rxCount < MESH_RXQ_SIZE) {
            memcpy(rxQ[rxHead], data, len);
            rxLen[rxHead] = (uint8_t)len;
            rxChan[rxHead] = radioCh;
            memcpy(rxFrom[rxHead], senderMac, 6);
            rxHead = (rxHead + 1) % MESH_RXQ_SIZE;
            rxCount++;
            meshRxQueued++;
        }
        portEXIT_CRITICAL(&rxMux);
        return;
    }
    if (!meshStarted || len < MESH_MIN_LEN) { meshRxShort++; return; }
    if (data[0] != MESH_MAGIC0 || data[1] != MESH_MAGIC1) { meshRxBad++; return; }

    if (data[2] != MESH_VERSION) { meshRxBad++; return; }
    uint8_t type = data[3];
    if (type == MESH_T_ANNOUNCE && len < MESH_MIN_LEN + 1) { meshRxShort++; return; }
    if (type == MESH_T_DETECT && len < MESH_DETECT_LEN) { meshRxShort++; return; }
    if (type == MESH_T_COMMAND && len < MESH_CMD_MIN) { meshRxShort++; return; }
    if (type == MESH_T_ACK && len < MESH_ACK_LEN) { meshRxShort++; return; }
    if (type == MESH_T_BYE && len < MESH_BYE_LEN) { meshRxShort++; return; }
    if (type == MESH_T_DETACK && len < MESH_DETACK_LEN) { meshRxShort++; return; }
    if (type == MESH_T_STATUS && len < MESH_MIN_LEN + 1) { meshRxShort++; return; }
    if (type == MESH_T_NACK && len < MESH_NACK_LEN) { meshRxShort++; return; }
    if (memcmp(data + 6, selfMac, 6) == 0) { meshRxSelf++; return; }  // our own packet relayed back

    if (meshSeen(data + 6, data[5], millis())) { meshRxDup++; return; }

    portENTER_CRITICAL(&rxMux);
    if (rxCount < MESH_RXQ_SIZE) {
        uint8_t l = (uint8_t)min(len, MESH_PKT_MAX);
        memcpy(rxQ[rxHead], data, l);
        rxLen[rxHead] = l;
        rxChan[rxHead] = radioCh;
        memcpy(rxFrom[rxHead], senderMac, 6);
        rxHead = (rxHead + 1) % MESH_RXQ_SIZE;
        rxCount++;
        meshRxQueued++;
    }
    portEXIT_CRITICAL(&rxMux);
}

static void meshSendCb(const uint8_t* mac_addr, esp_now_send_status_t status) {
    txInflight = false;   // the radio is free for the next frame
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
    {
        esp_err_t re = esp_wifi_config_espnow_rate(WIFI_IF_STA, (wifi_phy_rate_t)MESH_ESPNOW_LR_RATE);
        meshLrRateOk = (re == ESP_OK);
        if (isSerialConnected()) Serial.printf("[MESH] ESP-NOW TX rate: LR (phy rate %d): %s\n",
                                               (int)MESH_ESPNOW_LR_RATE, esp_err_to_name(re));
    }
#else
    if (isSerialConnected()) Serial.println("[MESH] ESP-NOW TX rate: driver default (1 Mbps, not LR)");
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
    memset(pendDet, 0, sizeof(pendDet));
    stageReset();
    burstRemaining = 0;
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
}

// Radio duty accounting: how much of the time this node actually sits on
// the mesh channel (and how much of that is shared with a BLE scan, which
// coexistence gives most of the airtime). Two nodes only hear each other
// while both are parked, so these numbers bound the link. Rolling 10 s.
static uint32_t dutyAt = 0, dutyParkMs = 0, dutyBleMs = 0, dutyWinAt = 0;
static bool dutyBle = false;
static uint8_t dutyParkPct = 0, dutyBlePct = 0;

static void dutyAccount(uint32_t now) {
    if (meshParked) {
        uint32_t d = now - dutyAt;
        dutyParkMs += d;
        if (dutyBle) dutyBleMs += d;
    }
    dutyAt = now;
}

void meshSetBlePhase(bool scanning) {
    dutyAccount(millis());
    dutyBle = scanning;
}

void meshDutyStats(uint8_t& parkPct, uint8_t& parkBlePct) {
    parkPct = dutyParkPct;
    parkBlePct = dutyBlePct;
}

static void dutyTick(uint32_t now) {
    if (now - dutyWinAt < 10000) return;
    dutyAccount(now);
    uint32_t win = now - dutyWinAt;
    dutyParkPct = (uint8_t)min(100UL, (unsigned long)(dutyParkMs * 100 / win));
    dutyBlePct = (uint8_t)min(100UL, (unsigned long)(dutyBleMs * 100 / win));
    dutyParkMs = dutyBleMs = 0;
    dutyWinAt = now;
}

void meshParkChannel() {
    if (!meshStarted) return;
    esp_wifi_set_channel(meshCfg.channel, WIFI_SECOND_CHAN_NONE);
    radioCh = meshCfg.channel;
    dutyAccount(millis());
    meshParked = true;
}

void meshUnparkChannel() { dutyAccount(millis()); meshParked = false; }

void meshNoteRadioChannel(uint8_t ch) { radioCh = ch; }

// Narrower than meshUrgent(): only work that justifies pausing BLE — a
// command going out, a reply owed or expected, a fresh detection. Routine
// announces sitting in the queue do NOT count (they are almost always
// there, and counting them starved BLE completely).
bool meshNeedsAir() {
    if (!meshStarted) return false;
    uint32_t now = millis();
    if (outActive || outRepeatLeft || outResendMask || (int32_t)(listenUntil - now) > 0) return true;
    for (int i = 0; i < MESH_DET_PENDING; i++) {
        if (pendDet[i].used && now - pendDet[i].firstAt < MESH_URGENT_MS) return true;
    }
    return false;
}

void meshHoldRadio(uint32_t ms) {
    uint32_t until = millis() + ms;
    if ((int32_t)(until - listenUntil) > 0) listenUntil = until;
}

const char* meshTxRateName() {
#ifdef MESH_ESPNOW_LR_RATE
    if (meshLrRateOk) return (MESH_ESPNOW_LR_RATE == WIFI_PHY_RATE_LORA_250K) ? "lr250k" : "lr500k";
    return "lr-failed";
#else
    return "1m";
#endif
}

void meshStraysJson(String& out) {
    uint32_t now = millis();
    out = "[";
    bool first = true;
    for (int i = 0; i < MESH_STRAY_SLOTS; i++) {
        if (!strays[i].ts || now - strays[i].ts > 60000) continue;
        char b[64];
        snprintf(b, sizeof(b), "%s{\"id\":\"%02X%02X%02X\",\"ch\":%u,\"last\":%lu}",
                 first ? "" : ",", strays[i].id[0], strays[i].id[1], strays[i].id[2],
                 strays[i].ch, (unsigned long)((now - strays[i].ts) / 1000));
        out += b;
        first = false;
    }
    out += "]";
}

static void strayRecord(const uint8_t* id, uint8_t ch, uint8_t role, uint32_t now) {
    int slot = 0;
    for (int i = 0; i < MESH_STRAY_SLOTS; i++) {
        if (strays[i].ts && memcmp(strays[i].id, id, 3) == 0) { slot = i; break; }
        if (strays[i].ts < strays[slot].ts) slot = i;
    }
    bool fresh = !(strays[slot].ts && memcmp(strays[slot].id, id, 3) == 0 &&
                   strays[slot].ch == ch && now - strays[slot].ts < 30000);
    memcpy(strays[slot].id, id, 3);
    strays[slot].ch = ch;
    if (role != 0xFF) strays[slot].role = role;
    strays[slot].ts = now ? now : 1;
    if (fresh && isSerialConnected()) {
        Serial.printf("[MESH] node %02X%02X%02X heard on channel %u — fleet channel is %u\n",
                      id[0], id[1], id[2], ch, meshCfg.channel);
    }
}

static void meshRelayNow(uint8_t* pkt, uint8_t len) {
    if (pkt[4] > 1 && txCount < MESH_TXQ_SIZE) {
        pkt[4]--;
        sealRelay = true;    // keep the origin's boot/ctr (replay windows key on it)
        if (!meshTxPush(pkt, len)) pkt[4]++;  // no slot: don't burn the ttl
        sealRelay = false;
    }
}

// Managed flood: every node is a repeater, but not every node has to
// speak. A relay waits briefly, and the wait follows the link the frame
// arrived on: heard WEAKLY means it came from far away, so this node
// extends it furthest and goes first; heard strongly means it came from
// next door, so it waits. Once MESH_RELAY_ENOUGH other nodes have carried
// the same frame, ours is dropped. The first copy to reach any node rides
// the longest working hops (fewest hops to the far end), and nobody keeps
// routes: a node that dies or moves just means the next copy goes another
// way. Detections and acks keep their own end-to-end retries on top.
#define MESH_RELAY_SLOTS   12
#define MESH_RELAY_ENOUGH  2       // keep redundancy: two carriers, not one
#define MESH_RELAY_MAX_MS  4000UL  // a relay older than this is stale
struct MeshRelayPend {
    uint8_t pkt[MESH_PKT_MAX]; uint8_t len; uint32_t dueAt, at; bool used;
    uint16_t boot; uint32_t ctr;   // origin's seal counters, carried into the re-seal
};
static MeshRelayPend relayPend[MESH_RELAY_SLOTS];
uint32_t meshRelaySent = 0, meshRelaySuppressed = 0;

// Link signal from the sniff table (sweep phases only); 0 when unknown.
static int8_t meshNeighborRssi(const uint8_t* mac) {
    uint32_t now = millis();
    for (int i = 0; i < MESH_SNIFF_SLOTS; i++) {
        // Signed: the sniffer callback can stamp sniffTs after `now` was read.
        if (sniffTs[i] && (int32_t)(now - sniffTs[i]) < 60000 && memcmp(sniffMac[i], mac, 6) == 0) return sniffRssi[i];
    }
    return 0;
}

// -95 dBm (edge of range) -> ~0 ms, -40 dBm (next door) -> ~110 ms,
// unknown -> the middle; plus jitter so equal links don't collide.
static uint32_t meshRelayDelayMs(int8_t rssi) {
    int r = rssi ? rssi : -70;
    if (r < -95) r = -95;
    if (r > -40) r = -40;
    return (uint32_t)((r + 95) * 2) + (esp_random() % 25);
}

static void meshRelay(uint8_t* pkt, uint8_t len, const uint8_t* fromMac) {
    if (pkt[4] <= 1) return;
    uint32_t now = millis();
    for (int i = 0; i < MESH_RELAY_SLOTS; i++) {
        if (relayPend[i].used) continue;
        memcpy(relayPend[i].pkt, pkt, len);
        relayPend[i].len = len;
        relayPend[i].at = now;
        relayPend[i].boot = curBoot;
        relayPend[i].ctr = curCtr;
        relayPend[i].dueAt = now + meshRelayDelayMs(meshNeighborRssi(fromMac));
        relayPend[i].used = true;
        return;
    }
    meshRelayNow(pkt, len);   // table full: plain flood
}

static bool meshRelaysPending() {
    for (int i = 0; i < MESH_RELAY_SLOTS; i++) if (relayPend[i].used) return true;
    return false;
}

// Radio parked: move due relays into the TX queue, or drop the ones
// enough other nodes have already carried.
static void meshRelayTick(uint32_t now) {
    for (int i = 0; i < MESH_RELAY_SLOTS; i++) {
        MeshRelayPend& r = relayPend[i];
        if (!r.used) continue;
        if (now - r.at > MESH_RELAY_MAX_MS) { r.used = false; continue; }
        if ((int32_t)(now - r.dueAt) < 0) continue;
        if (meshSeenCopies(r.pkt + 6, r.pkt[5]) >= MESH_RELAY_ENOUGH) {
            r.used = false;
            meshRelaySuppressed++;
            continue;
        }
        if (txCount >= MESH_TXQ_SIZE) return;   // try again next flush
        curBoot = r.boot;
        curCtr = r.ctr;
        meshRelayNow(r.pkt, r.len);
        r.used = false;
        meshRelaySent++;
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
        uint8_t rxHop[6];
        uint8_t len = 0, ch = 0;
        portENTER_CRITICAL(&rxMux);
        if (rxCount > 0) {
            memcpy(pkt, rxQ[rxTail], rxLen[rxTail]);
            len = rxLen[rxTail];
            ch = rxChan[rxTail];
            memcpy(rxHop, rxFrom[rxTail], 6);
            rxTail = (rxTail + 1) % MESH_RXQ_SIZE;
            rxCount--;
        }
        portEXIT_CRITICAL(&rxMux);
        if (len == 0) break;

        if (encReady) {
            // Sealed: open it, then the checks the receive callback can't
            // do in the clear any more (self, duplicates, replays, length).
            uint8_t frame[MESH_PKT_MAX];
            memcpy(frame, pkt, len);
            if (!encOpen(frame, len, pkt, len, curBoot, curCtr, now)) continue;
            if (memcmp(pkt + 6, selfMac, 6) == 0) { meshRxSelf++; continue; }
            if (meshSeen(pkt + 6, pkt[5], now)) { meshRxDup++; continue; }
            if (!replayFresh(pkt + MESH_HDR_LEN, curBoot, curCtr, now)) { encDropReplay++; continue; }
            if (!meshLenOk(pkt[3], len)) { meshRxShort++; continue; }
        }

        uint8_t type = pkt[3];
        const uint8_t* nodeId = pkt + MESH_HDR_LEN;
        bool isAnnounce = (type == MESH_T_ANNOUNCE);
        uint8_t role = isAnnounce ? pkt[MESH_HDR_LEN + 3] : 0xFF;
        // Announces from current builds state the sender's channel; that
        // beats the radio channel (adjacent-channel leakage fakes links).
        if (isAnnounce && len >= MESH_MIN_LEN + 2) {
            uint8_t stated = pkt[MESH_HDR_LEN + 4];
            if (stated >= MESH_CH_MIN && stated <= MESH_CH_MAX) ch = stated;
        }

        // Same channel as ours (or a heal step listening on healCh, or
        // channel unknown) = a real link. Anything else is a stray: its
        // payload still counts (a detection is a detection), but it is not
        // presence, and it doesn't reset the heal silence timer.
        bool sameCh = ch == 0 || ch == meshCfg.channel || (healActive && ch == healCh);
        if (!sameCh) {
            strayRecord(nodeId, ch, role, now);
            bool stated = isAnnounce && len >= MESH_MIN_LEN + 2;
            if (!meshCfg.home && isAnnounce && role == 1 && !healActive &&
                (stated || now - homeLinkedAt > MESH_FOLLOW_WINDOW_MS)) {
                if (followCh != ch || now - followFirstAt > MESH_FOLLOW_WINDOW_MS) {
                    followCh = ch;
                    followHits = 0;
                    followFirstAt = now;
                }
                if (++followHits >= MESH_FOLLOW_HITS) {
                    if (isSerialConnected()) {
                        Serial.printf("[MESH] home %02X%02X%02X is on channel %u — following it (was %u)\n",
                                      nodeId[0], nodeId[1], nodeId[2], ch, meshCfg.channel);
                    }
                    meshCfg.channel = ch;
                    mesh_saveConfig();
                    for (int i = 0; i < MESH_ROSTER_SIZE; i++) roster[i].live = false;
                    followHits = 0;
                    lastTrafficAt = now;
                    meshParkChannel();
                    meshStartBurst(now);
                }
            }
            if (isAnnounce) continue;   // an announce carries nothing else
        } else {
            lastTrafficAt = now;
            if (role == 1) homeLinkedAt = now;
        }

        bool isNewNode = sameCh && meshRosterUpdate(nodeId, isAnnounce, role, now);
        if (sameCh) {
            bool direct = memcmp(rxHop, pkt + 6, 6) == 0;   // sent by the origin itself
            for (int i = 0; i < MESH_ROSTER_SIZE; i++) {
                if (roster[i].live && memcmp(roster[i].id, nodeId, 3) == 0) {
                    memcpy(roster[i].mac, pkt + 6, 6);
                    if (direct) roster[i].lastDirect = now;
                    break;
                }
            }
        }
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
            bool upd      = (body[5] & 0x02) != 0;
            char mac[19], ident[19], desc[41];
            strlcpy(mac,   (const char*)(body + 6),  sizeof(mac));
            strlcpy(ident, (const char*)(body + 24), sizeof(ident));
            strlcpy(desc,  (const char*)(body + 42), sizeof(desc));

            // Desc arrives sender-truncated to 40 chars; append the mesh
            // provenance inside main.cpp's 64-char description budget.
            char full[64];
            snprintf(full, sizeof(full), "%.40s [mesh %02X%02X%02X]",
                     desc, nodeId[0], nodeId[1], nodeId[2]);
            if (len >= MESH_DETECT_ID_LEN) {
                // Reliable path: alert once per (origin, detId); a home
                // acks every copy (an earlier ack may have been lost).
                uint16_t detId = body[82] | (body[83] << 8);
                if (detSeenFirst(nodeId, detId, now)) meshReport(mac, ident, full, rssi, dtype, wifiDom, upd);
                if (meshCfg.home) meshSendDetAck(nodeId, detId);
            } else {
                meshReport(mac, ident, full, rssi, dtype, wifiDom, false);   // legacy sender
            }
            meshRelay(pkt, len, rxHop);
            break;
        }
        case MESH_T_DETACK: {
            const uint8_t* body = pkt + MESH_HDR_LEN;
            if (memcmp(body + 3, selfId, 3) == 0) {
                uint16_t detId = body[6] | (body[7] << 8);
                for (int i = 0; i < MESH_DET_PENDING; i++) {
                    if (pendDet[i].used && pendDet[i].detId == detId) {
                        pendDet[i].used = false;
                        detAcked++;
                    }
                }
            } else {
                meshRelay(pkt, len, rxHop);   // carry it back toward its origin
            }
            break;
        }
        case MESH_T_COMMAND:
            meshRelayNow(pkt, len);   // relay first, at once: a reboot command still spreads
            meshHandleCommand(pkt, len);
            break;
        case MESH_T_ACK: {
            uint16_t ackId = pkt[MESH_HDR_LEN + 4] | (pkt[MESH_HDR_LEN + 5] << 8);
            if (ackFirst(nodeId, ackId, now)) {   // count + report each node once
                meshAckRecord(ackId, pkt[MESH_HDR_LEN + 6]);
                meshOnAck(nodeId, pkt[MESH_HDR_LEN + 3], ackId,
                          pkt[MESH_HDR_LEN + 6], pkt[MESH_HDR_LEN + 7]);
            }
            meshRelay(pkt, len, rxHop);
            break;
        }
        case MESH_T_STATUS:
            meshOnStatus(nodeId, pkt + MESH_HDR_LEN + 3, len - (MESH_HDR_LEN + 3));
            meshRelay(pkt, len, rxHop);
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
        case MESH_T_OTA: {
            // Body: senderId[3], sub, session(2), dest[3], ... (mesh_ota.cpp).
            // Frames for another node are relayed at once (no managed-flood
            // wait: an update is thousands of frames); the sender sends with
            // ttl 1 when the target is in direct range, so nothing relays then.
            const uint8_t* body = pkt + MESH_HDR_LEN;
            if (memcmp(body + 6, selfId, 3) == 0) meshOtaRx(body, len - MESH_HDR_LEN, pkt + 6, rxHop);
            else meshRelayNow(pkt, len);
            break;
        }
        case MESH_T_NACK: {
            // Body: requester[3], sender[3], cmdId(2), missing mask(4).
            const uint8_t* body = pkt + MESH_HDR_LEN;
            if (memcmp(body + 3, selfId, 3) == 0) meshOnNack(body);
            else meshRelay(pkt, len, rxHop);
            break;
        }
        case MESH_T_ANNOUNCE:
        default:
            break;  // announces carry ttl=1 and are never relayed
        }
    }

    dutyTick(millis());   // park/unpark in the drain stamped dutyAt later than `now`

    // Stale partial transfer cleanup.
    // Signed and against the clock now: `now` was read at the top of this
    // tick, and a chunk handled in the drain above stamps stageLastChunk
    // with a later millis(). Unsigned `now - stageLastChunk` then wrapped
    // to ~4e9, read as "stale", and threw away every partly-received
    // multi-chunk command (filter reports from remotes never arrived).
    if (stageActive && (int32_t)(millis() - stageLastChunk) > (int32_t)MESH_STAGE_GAP_MS) stageReset();
    // Selective repeat: rounds over and still holes? Name them to the sender.
    static uint16_t nackFor = 0;
    static uint8_t  nackTries = 0;
    static uint32_t nackAt = 0;
    if (stageActive && stageChunks) {
        if (nackFor != stageCmdId) { nackFor = stageCmdId; nackTries = 0; nackAt = 0; }
        uint32_t t = millis();
        if ((int32_t)(t - stageLastChunk) > MESH_NACK_QUIET_MS && nackTries < MESH_NACK_TRIES &&
            (!nackAt || (int32_t)(t - nackAt) > MESH_NACK_QUIET_MS)) {
            uint32_t want;
            if (stageHaveFinal) {
                uint16_t lastIdx = stageTotal == 0 ? 0 : (stageTotal - 1) / MESH_CMD_CHUNK;
                uint32_t need = (lastIdx >= 31) ? 0xFFFFFFFFUL : ((1UL << (lastIdx + 1)) - 1);
                want = need & ~stageChunks;
            } else {
                want = ~stageChunks;   // the tail too: the sender trims to its length
            }
            if (want) {
                meshSendNack(stageOrigin, stageCmdId, want);
                meshHoldRadio(3000);
                nackTries++;
                nackAt = t;
            }
        }
    }
    static uint16_t stalledLogged = 0;
    if (stageActive && stalledLogged != stageCmdId && (int32_t)(millis() - stageLastChunk) > 8000) {
        stalledLogged = stageCmdId;
        if (isSerialConnected()) {
            Serial.printf("[MESH] transfer %02X%02X%02X/%u stalled 8 s: chunks %08lX%s\n",
                          stageOrigin[0], stageOrigin[1], stageOrigin[2], stageCmdId,
                          (unsigned long)stageChunks, stageHaveFinal ? " (final seen)" : "");
        }
    }

    // Channel auto-heal: silent too long while meshing → walk the mesh
    // channels (1-11, the same range the config allows), listening on each.
    // Any mesh traffic adopts that channel permanently. The loop() bypasses
    // the scan phase machine while this runs so nothing fights the changes.
    // The HOME is the channel anchor: it never wanders (its channel changes
    // only by meshrechan, which takes the whole fleet along). Remotes heal.
    if (healAllowed && !meshCfg.home && !healActive && meshNodeCount() == 0 &&
        now - lastTrafficAt > MESH_HEAL_AFTER_MS) {
        healActive = true;
        healCh = MESH_CH_MIN;
        healStepAt = now;
        esp_wifi_set_channel(healCh, WIFI_SECOND_CHAN_NONE);
        radioCh = healCh;
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
            if (healCh >= MESH_CH_MAX) {
                // One full pass found nobody: go home to the saved channel
                // and try again after another quiet spell, so a node whose
                // fleet is simply switched off doesn't stop scanning for good.
                healActive = false;
                lastTrafficAt = now;
                meshParkChannel();
                if (isSerialConnected()) Serial.println("[MESH] heal pass found no fleet — back to saved channel");
            } else {
                healCh++;
                healStepAt = now;
                esp_wifi_set_channel(healCh, WIFI_SECOND_CHAN_NONE);
                radioCh = healCh;
            }
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
