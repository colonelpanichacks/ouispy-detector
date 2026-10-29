// Firmware updates over the ESP-NOW mesh. Protocol overview in mesh_ota.h.
#include "mesh_ota.h"
#include "mesh.h"
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>

bool isSerialConnected();

#define OTA_CHUNK      192     // data bytes per frame (sealed frame stays <= 250)
#define OTA_WIN        32      // chunks per window (bitmap is a u32)
#define OTA_SUB_DATA   1       // home -> target: idx(2) data
#define OTA_SUB_STATE  2       // target -> home (v1): base(2) bitmap(4) status(1) err(1)
#define OTA_SUB_POLL   3       // home -> target: "where are you?"
#define OTA_SUB_ABORT  4       // home -> target
#define OTA_SUB_STATE2 5       // target -> home (v2): count(2) status err n, n x (start(2) len(2)) missing
#define OTA_RANGES     40      // missing ranges per STATE2 (keeps the sealed frame < 250)
#define OTA_ST_RECV    0
#define OTA_ST_DONE    3
#define OTA_ST_FAIL    4
#define OTA_HDR        9       // senderId(3) sub(1) session(2) dest(3)

// Failure codes in STATE (also printed by the home).
enum { OTA_E_NONE, OTA_E_WRITE, OTA_E_HASH, OTA_E_IMAGE, OTA_E_BOOT, OTA_E_ABORT, OTA_E_TIMEOUT };
static const char* const OTA_ERR[] = { "", "flash write failed", "SHA-256 mismatch",
                                       "image rejected by ESP-IDF", "could not set boot partition",
                                       "aborted", "timed out" };

static void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF; }
static uint16_t get16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void hexId(const uint8_t id[3], char out[7]) { snprintf(out, 7, "%02X%02X%02X", id[0], id[1], id[2]); }

static void otaHeader(uint8_t* b, uint8_t sub, uint16_t session, const uint8_t dest[3]) {
    meshSelfIdBytes(b);
    b[3] = sub;
    put16(b + 4, session);
    memcpy(b + 6, dest, 3);
}

// =====================================================================
// Home: staging (image arrives over USB into the spare partition)
// =====================================================================
static const esp_partition_t* stagePart = nullptr;
static uint32_t stageSize = 0, stageWritten = 0;
static uint8_t stageSha[32];
static uint8_t* stageBuf = nullptr;          // whole image in PSRAM while it arrives
static bool stageOpen = false, stageReady = false;
static uint32_t stageLastAt = 0;
static mbedtls_sha256_context stageCtx;

// The image is collected in PSRAM and only written to flash at fwend, when
// the USB link is quiet: flash writes while USB data was streaming in reset
// the S3 (the home dropped off USB a few KB into staging).
static void stageDrop() {
    if (stageOpen) mbedtls_sha256_free(&stageCtx);
    stageOpen = false;
    if (stageBuf) { free(stageBuf); stageBuf = nullptr; }
}

bool otaStageBegin(uint32_t size, const uint8_t sha[32], String& err) {
    stageDrop();
    stageReady = false;
    stagePart = esp_ota_get_next_update_partition(nullptr);
    if (!stagePart) { err = "no OTA partition (flash this node once over USB with the OTA layout)"; return false; }
    if (size == 0 || size > stagePart->size) { err = "image does not fit the OTA partition"; return false; }
    stageBuf = (uint8_t*)ps_malloc(size);
    if (!stageBuf) { err = "no PSRAM for the image"; return false; }
    stageSize = size;
    stageWritten = 0;
    memcpy(stageSha, sha, 32);
    mbedtls_sha256_init(&stageCtx);
    mbedtls_sha256_starts(&stageCtx, 0);
    stageOpen = true;
    stageLastAt = millis();
    return true;
}

bool otaStageData(uint32_t offset, const uint8_t* data, uint16_t len, String& err) {
    if (!stageOpen) { err = "no fwbegin"; return false; }
    if (offset != stageWritten) { err = "expected offset " + String(stageWritten); return false; }
    if (stageWritten + len > stageSize) { err = "past the announced size"; return false; }
    memcpy(stageBuf + stageWritten, data, len);
    mbedtls_sha256_update(&stageCtx, data, len);
    stageWritten += len;
    stageLastAt = millis();
    return true;
}

bool otaStageEnd(String& err) {
    if (!stageOpen) { err = "no fwbegin"; return false; }
    uint8_t got[32];
    mbedtls_sha256_finish(&stageCtx, got);
    mbedtls_sha256_free(&stageCtx);
    stageOpen = false;
    bool ok = false;
    esp_ota_handle_t h = 0;
    if (stageWritten != stageSize) err = "short image";
    else if (memcmp(got, stageSha, 32) != 0) err = "SHA-256 mismatch";
    else if (esp_ota_begin(stagePart, stageSize, &h) != ESP_OK) err = "esp_ota_begin failed";
    else {
        ok = true;
        for (uint32_t off = 0; off < stageSize && ok; off += 4096) {
            uint32_t n = stageSize - off < 4096 ? stageSize - off : 4096;
            if (esp_ota_write(h, stageBuf + off, n) != ESP_OK) { err = "flash write failed"; ok = false; }
        }
        if (!ok) esp_ota_abort(h);
        else if (esp_ota_end(h) != ESP_OK) { err = "image rejected by ESP-IDF (not a valid ESP32-S3 app?)"; ok = false; }
    }
    free(stageBuf);
    stageBuf = nullptr;
    stageReady = ok;
    return ok;
}

// An upload the dashboard abandoned (closed, crashed) must not hold the
// node in update mode forever: after a minute of silence, drop it.
static void stageTick(uint32_t now) {
    if (stageOpen && now - stageLastAt > 60000) {
        stageDrop();
        if (isSerialConnected()) Serial.println("[OTA] firmware upload abandoned (no data for 60 s)");
    }
}

bool otaStaged(uint32_t& size) { size = stageSize; return stageReady; }

bool otaSelfInstall(String& err) {
    if (!stageReady) { err = "nothing staged"; return false; }
    if (esp_ota_set_boot_partition(stagePart) != ESP_OK) { err = "could not set boot partition"; return false; }
    return true;   // caller announces and restarts
}

// =====================================================================
// Home: sending one staged image to one target
// =====================================================================
// Two receivers exist in the field. v1 (the first OTA build) takes a window
// of 32 chunks at a time and reports it with STATE. v2 holds the whole image
// in PSRAM, takes chunks in any order and reports missing ranges with
// STATE2; the sender then streams the image once and repairs what's
// missing, with no stop-and-wait (v1 crawled at ~5 kbps on a weak link).
// The sender learns which one it talks to from the first report.
enum { TX_IDLE, TX_OFFER, TX_SEND, TX_DONE, TX_FAIL };
static uint8_t txState = TX_IDLE;
static uint8_t txProto = 0;                      // 1 = v1 windows, 2 = v2 stream
static uint8_t txTarget[3];
static uint16_t txSession;
static uint32_t txChunks, txBase, txAcked;       // v1: window base; acked bitmap of it
static uint32_t txSentMask;                      // v1: sent in the current round
static uint32_t txCursor, txGot;                 // v2: next chunk of the first pass; chunks the target has
static uint16_t txMiss[OTA_RANGES][2];           // v2: missing ranges from the last STATE2
static uint8_t txMissN, txMissI;
static uint16_t txMissOff;
static uint32_t txOfferAt, txLastStateAt, txLastPushAt, txPollAt, txStartAt, txReportAt, txDirectAt;
static uint8_t txPolls;
static bool txDirect = false;                    // target heard first-hand lately
static char txErr[48];

static uint32_t winSize(uint32_t base) { uint32_t left = txChunks - base; return left < OTA_WIN ? left : OTA_WIN; }
static uint32_t winFull(uint32_t base) { uint32_t n = winSize(base); return n >= 32 ? 0xFFFFFFFFUL : ((1UL << n) - 1); }

static void txReport(const char* state) {
    if (!isSerialConnected()) return;
    char id[7];
    hexId(txTarget, id);
    uint32_t done = (txProto == 2 ? txGot : txBase) * OTA_CHUNK;
    if (done > stageSize || txState == TX_DONE) done = stageSize;
    uint32_t pct = stageSize ? (uint32_t)((uint64_t)done * 100 / stageSize) : 0;
    uint32_t secs = (millis() - txStartAt) / 1000;
    uint32_t kbps = secs ? (done * 8 / 1000) / secs : 0;
    Serial.printf("OTA {\"node\":\"%s\",\"state\":\"%s\",\"pct\":%u,\"bytes\":%u,\"size\":%u,\"kbps\":%u,\"direct\":%d,\"proto\":%u,\"err\":\"%s\"}\n",
                  id, state, pct, done, stageSize, kbps, txDirect ? 1 : 0, txProto, txErr);
}

static void txFail(const char* why) {
    strlcpy(txErr, why, sizeof(txErr));
    txState = TX_FAIL;
    uint8_t b[OTA_HDR];
    otaHeader(b, OTA_SUB_ABORT, txSession, txTarget);
    meshOtaSend(b, sizeof(b), true);
    txReport("failed");
}

uint16_t otaOfferPayload(const uint8_t target[3], uint8_t* out) {
    // {session u16, size u32, sha[32], home id[3], proto u8}. v1 receivers
    // read the first 41 bytes and ignore the rest.
    uint8_t home[3];
    meshSelfIdBytes(home);
    uint16_t session = (uint16_t)random(1, 0x10000);
    put16(out, session);
    put32(out + 2, stageSize);
    memcpy(out + 6, stageSha, 32);
    memcpy(out + 38, home, 3);
    out[41] = 2;
    return 42;
}

void otaSendBegin(const uint8_t target[3], uint16_t session) {
    memcpy(txTarget, target, 3);
    txSession = session;
    txChunks = (stageSize + OTA_CHUNK - 1) / OTA_CHUNK;
    txBase = txAcked = txSentMask = 0;
    txCursor = txGot = 0;
    txMissN = txMissI = 0;
    txMissOff = 0;
    txProto = 0;
    txPolls = 0;
    txDirect = false;
    txErr[0] = 0;
    txOfferAt = txStartAt = txReportAt = millis();
    txLastStateAt = txLastPushAt = txPollAt = txDirectAt = 0;
    txState = TX_OFFER;
    txReport("offered");
}

void otaSendAbort() {
    if (txState == TX_OFFER || txState == TX_SEND) txFail("aborted");
}

// Common to both report kinds: who sent it, first-hand or relayed.
static bool txStateFor(const uint8_t* b, const uint8_t* originMac, const uint8_t* rxHop) {
    if (txState != TX_OFFER && txState != TX_SEND) return false;
    if (memcmp(b, txTarget, 3) != 0 || get16(b + 4) != txSession) return false;
    uint32_t now = millis();
    if (memcmp(originMac, rxHop, 6) == 0) txDirectAt = now ? now : 1;
    // Sticky for a few seconds: one relayed copy doesn't flip every data
    // frame to relaying (and doubling the airtime).
    txDirect = txDirectAt && now - txDirectAt < 8000;
    txLastStateAt = now;
    txPolls = 0;
    return true;
}

static bool txStatusDone(uint8_t status, uint8_t err) {
    if (status == OTA_ST_FAIL) { txFail(err < sizeof(OTA_ERR) / sizeof(OTA_ERR[0]) ? OTA_ERR[err] : "target failed"); return true; }
    if (status == OTA_ST_DONE) { txBase = txGot = txChunks; txState = TX_DONE; txReport("done"); return true; }
    if (txState == TX_OFFER) txState = TX_SEND;
    return false;
}

static void txOnState(const uint8_t* b, uint8_t len, const uint8_t* originMac, const uint8_t* rxHop) {   // v1
    if (len < OTA_HDR + 8 || !txStateFor(b, originMac, rxHop)) return;
    txProto = 1;
    uint32_t base = get16(b + OTA_HDR), bitmap = get32(b + OTA_HDR + 2);
    if (txStatusDone(b[OTA_HDR + 6], b[OTA_HDR + 7])) return;
    if (base > txBase) { txBase = base; txAcked = bitmap; txSentMask = 0; }
    else if (base == txBase) { txAcked = bitmap; txSentMask = txAcked; }   // resend only what's missing
}

static void txOnState2(const uint8_t* b, uint8_t len, const uint8_t* originMac, const uint8_t* rxHop) {  // v2
    if (len < OTA_HDR + 5 || !txStateFor(b, originMac, rxHop)) return;
    txProto = 2;
    txGot = get16(b + OTA_HDR);
    if (txStatusDone(b[OTA_HDR + 2], b[OTA_HDR + 3])) return;
    uint8_t n = b[OTA_HDR + 4];
    if (n > OTA_RANGES || len < OTA_HDR + 5 + n * 4) return;
    for (uint8_t k = 0; k < n; k++) {
        txMiss[k][0] = get16(b + OTA_HDR + 5 + k * 4);
        txMiss[k][1] = get16(b + OTA_HDR + 7 + k * 4);
    }
    txMissN = n;
    txMissI = 0;
    txMissOff = 0;
}

static bool txSendChunk(uint32_t idx, uint32_t now) {
    uint8_t b[OTA_HDR + 2 + OTA_CHUNK];
    otaHeader(b, OTA_SUB_DATA, txSession, txTarget);
    put16(b + OTA_HDR, (uint16_t)idx);
    uint32_t off = idx * OTA_CHUNK;
    uint16_t n = (off + OTA_CHUNK <= stageSize) ? OTA_CHUNK : (uint16_t)(stageSize - off);
    if (esp_partition_read(stagePart, off, b + OTA_HDR + 2, n) != ESP_OK) { txFail("staged image unreadable"); return false; }
    if (!meshOtaSend(b, OTA_HDR + 2 + n, !txDirect)) return false;
    txLastPushAt = now;
    return true;
}

static void txPoll(uint32_t now, uint16_t limit) {
    if (++txPolls > limit) { txFail("target went silent"); return; }
    uint8_t b[OTA_HDR];
    otaHeader(b, OTA_SUB_POLL, txSession, txTarget);
    meshOtaSend(b, sizeof(b), !txDirect || txPolls > 3);   // after a few misses, try relays too
    txPollAt = now;
}

static void txTick(uint32_t now) {
    if (txState == TX_OFFER) {
        meshHoldRadio(2000);
        if (now - txOfferAt > 60000) txFail("target never answered the offer");
        return;
    }
    if (txState != TX_SEND) return;
    meshHoldRadio(2000);
    if (now - txReportAt > 2000) { txReportAt = now; txReport("sending"); }
    // Progress watchdog. "Went silent" only catches a target that stops answering; one
    // that keeps answering but never confirms the image (e.g. the staged image changed
    // under the transfer) held the sender at 100% forever. No new chunk confirmed for 2 min
    // ends it with an error the dashboard can retry on.
    static uint16_t wdSession = 0;
    static uint32_t wdGot = 0, wdAt = 0;
    if (wdSession != txSession) wdSession = txSession, wdGot = UINT32_MAX, wdAt = now;
    uint32_t got = txProto == 2 ? txGot : txBase;
    if (got != wdGot) wdGot = got, wdAt = now;
    else if (now - wdAt > 120000) { txFail("no progress for 2 min (target never confirmed the image)"); return; }
    if (txProto == 1) {
        uint32_t need = winFull(txBase) & ~txAcked & ~txSentMask;
        // Two TX slots stay free for announces, acks and relays.
        while (need && meshTxFree() > 2) {
            uint32_t i = __builtin_ctz(need);
            if (!txSendChunk(txBase + i, now)) break;
            txSentMask |= 1UL << i;
            need &= ~(1UL << i);
        }
        // Window out, no report yet: ask (quickly; on a weak link reports get lost).
        if (!need && now - txLastPushAt > 300 && now - txLastStateAt > 300 && now - txPollAt > 400) txPoll(now, 100);
        return;
    }
    // v2: stream every chunk once, then resend the ranges the target reports missing.
    bool idle = false;
    while (meshTxFree() > 2) {
        if (txCursor < txChunks) {
            if (!txSendChunk(txCursor, now)) break;
            txCursor++;
        } else if (txMissI < txMissN) {
            uint32_t idx = txMiss[txMissI][0] + txMissOff;
            if (idx >= txChunks) { txMissI = txMissN; continue; }
            if (!txSendChunk(idx, now)) break;
            if (++txMissOff >= txMiss[txMissI][1]) { txMissI++; txMissOff = 0; }
        } else { idle = true; break; }
    }
    if (idle && now - txLastPushAt > 300 && now - txLastStateAt > 300 && now - txPollAt > 400) txPoll(now, 150);
}

void otaPrintStatus() {
    if (txState == TX_IDLE) {
        const esp_partition_t* run = esp_ota_get_running_partition();
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        if (run) esp_ota_get_state_partition(run, &st);
        Serial.printf("OTA {\"state\":\"idle\",\"staged\":%d,\"size\":%u,\"running\":\"%s\",\"pending\":%d}\n",
                      stageReady ? 1 : 0, stageSize, run ? run->label : "?", st == ESP_OTA_IMG_PENDING_VERIFY ? 1 : 0);
        return;
    }
    static const char* const names[] = { "idle", "offered", "sending", "done", "failed" };
    txReport(names[txState]);
}

// =====================================================================
// Target: receiving (v2: any order, whole image in PSRAM)
// =====================================================================
static bool rxActive = false;
static uint16_t rxSession;
static uint8_t rxHome[3];
static uint32_t rxSize, rxChunks, rxCount, rxLastAt, rxStateAt, rxDoneAt;
static uint8_t rxSha[32], rxStatus, rxErr;
static uint8_t* rxImg = nullptr;             // whole image in PSRAM; flash is written at the end
static uint8_t* rxBits = nullptr;            // one bit per chunk received
static esp_ota_handle_t rxHandle;
static const esp_partition_t* rxPart = nullptr;
static bool rxDirty = false;                 // got data since the last report

static void rxFree() {
    if (rxImg) { free(rxImg); rxImg = nullptr; }
    if (rxBits) { free(rxBits); rxBits = nullptr; }
}

static bool rxHas(uint32_t i) { return rxBits[i >> 3] & (1 << (i & 7)); }

// STATE2: {count u16, status, err, n, n x (start u16, len u16)}: the first
// OTA_RANGES runs of missing chunks.
static void rxSendState() {
    uint8_t b[OTA_HDR + 5 + OTA_RANGES * 4];
    otaHeader(b, OTA_SUB_STATE2, rxSession, rxHome);
    put16(b + OTA_HDR, (uint16_t)rxCount);
    b[OTA_HDR + 2] = rxStatus;
    b[OTA_HDR + 3] = rxErr;
    uint8_t n = 0;
    if (rxStatus == OTA_ST_RECV && rxBits) {
        for (uint32_t i = 0; i < rxChunks && n < OTA_RANGES; ) {
            if (rxHas(i)) { i++; continue; }
            uint32_t start = i;
            while (i < rxChunks && !rxHas(i) && i - start < 0xFFFF) i++;
            put16(b + OTA_HDR + 5 + n * 4, (uint16_t)start);
            put16(b + OTA_HDR + 7 + n * 4, (uint16_t)(i - start));
            n++;
        }
    }
    b[OTA_HDR + 4] = n;
    meshOtaSend(b, OTA_HDR + 5 + n * 4, true);
    rxStateAt = millis();
    rxDirty = false;
}

static void rxFinish(uint8_t err) {
    rxErr = err;
    rxStatus = err ? OTA_ST_FAIL : OTA_ST_DONE;
    rxFree();
    if (err) {
        rxActive = false;
        if (isSerialConnected()) Serial.printf("[OTA] update failed: %s\n", OTA_ERR[err]);
    } else {
        rxDoneAt = millis();
        if (isSerialConnected()) Serial.println("[OTA] update verified, restarting into it");
    }
    for (int i = 0; i < 3; i++) rxSendState();   // three copies: radios time-slice
}

bool meshOtaOffer(const uint8_t* p, uint16_t len) {
    if (len < 41 || !meshKeySet() || !meshSealed()) return false;   // updates need the fleet key
    uint16_t session = get16(p);
    uint32_t size = get32(p + 2);
    if (rxActive && session == rxSession && memcmp(p + 6, rxSha, 32) == 0) {
        rxSendState();      // same update offered again: resume where it stopped
        return true;
    }
    rxFree();
    rxActive = false;
    rxPart = esp_ota_get_next_update_partition(nullptr);
    if (!rxPart || size == 0 || size > rxPart->size) return false;
    uint32_t chunks = (size + OTA_CHUNK - 1) / OTA_CHUNK;
    rxImg = (uint8_t*)ps_malloc(size);
    rxBits = (uint8_t*)calloc((chunks + 7) / 8, 1);
    if (!rxImg || !rxBits) { rxFree(); return false; }
    rxSession = session;
    rxSize = size;
    rxChunks = chunks;
    rxCount = 0;
    memcpy(rxSha, p + 6, 32);
    memcpy(rxHome, p + 38, 3);
    rxStatus = OTA_ST_RECV;
    rxErr = OTA_E_NONE;
    rxDoneAt = 0;
    rxActive = true;
    rxLastAt = millis();
    if (isSerialConnected()) Serial.printf("[OTA] receiving %u-byte update from the home\n", size);
    rxSendState();
    return true;
}

static void rxOnData(const uint8_t* b, uint8_t len) {
    if (!rxActive || rxStatus != OTA_ST_RECV || get16(b + 4) != rxSession || len < OTA_HDR + 3) return;
    rxLastAt = millis();
    uint32_t idx = get16(b + OTA_HDR);
    if (idx >= rxChunks) return;
    uint16_t n = len - (OTA_HDR + 2);
    uint32_t expect = (idx == rxChunks - 1) ? rxSize - idx * OTA_CHUNK : OTA_CHUNK;
    if (n != expect || rxHas(idx)) return;
    memcpy(rxImg + idx * OTA_CHUNK, b + OTA_HDR + 2, n);
    rxBits[idx >> 3] |= 1 << (idx & 7);
    rxCount++;
    rxDirty = true;
    if (rxCount < rxChunks) return;
    // Everything is here: check it in RAM, then write the flash in one pass.
    uint8_t got[32];
    mbedtls_sha256(rxImg, rxSize, got, 0);
    if (memcmp(got, rxSha, 32) != 0) { rxFinish(OTA_E_HASH); return; }
    if (esp_ota_begin(rxPart, rxSize, &rxHandle) != ESP_OK) { rxFinish(OTA_E_WRITE); return; }
    for (uint32_t off = 0; off < rxSize; off += 4096) {
        uint32_t w = rxSize - off < 4096 ? rxSize - off : 4096;
        if (esp_ota_write(rxHandle, rxImg + off, w) != ESP_OK) { esp_ota_abort(rxHandle); rxFinish(OTA_E_WRITE); return; }
    }
    if (esp_ota_end(rxHandle) != ESP_OK) { rxFinish(OTA_E_IMAGE); return; }
    if (esp_ota_set_boot_partition(rxPart) != ESP_OK) { rxFinish(OTA_E_BOOT); return; }
    rxFinish(OTA_E_NONE);
}

void meshOtaRx(const uint8_t* b, uint8_t len, const uint8_t* originMac, const uint8_t* rxHop) {
    if (len < OTA_HDR) return;
    switch (b[3]) {
        case OTA_SUB_DATA:   rxOnData(b, len); break;
        case OTA_SUB_POLL:   if (rxActive && get16(b + 4) == rxSession) rxSendState(); break;
        case OTA_SUB_ABORT:
            if (rxActive && get16(b + 4) == rxSession) { rxActive = false; rxFree(); }
            break;
        case OTA_SUB_STATE:  txOnState(b, len, originMac, rxHop); break;
        case OTA_SUB_STATE2: txOnState2(b, len, originMac, rxHop); break;
    }
}

static void rxTick(uint32_t now) {
    if (!rxActive) return;
    meshHoldRadio(2000);
    if (rxStatus == OTA_ST_DONE) {
        if (now - rxDoneAt > 2500) {   // the DONE copies are out: boot the new image
            meshFlushTxNow();
            delay(200);
            ESP.restart();
        }
        return;
    }
    // Report what's missing: twice a second while data flows, and every 2 s
    // as a keepalive (a lost report then costs at most that long).
    if ((rxDirty && now - rxStateAt > 500) || now - rxStateAt > 2000) rxSendState();
    if (now - rxLastAt > 90000) {
        rxFree();
        rxActive = false;
        if (isSerialConnected()) Serial.println("[OTA] update timed out, back to scanning");
    }
}

void meshOtaTick() {
    uint32_t now = millis();
    stageTick(now);
    txTick(now);
    rxTick(now);
}

// Staging counts too: the scan loop then services the USB console every
// millisecond instead of once per scan step (staging crawled at ~4 lines/s).
bool meshOtaBusy() { return rxActive || stageOpen || txState == TX_OFFER || txState == TX_SEND; }

// =====================================================================
// Rollback guard: a freshly updated image must prove itself.
// =====================================================================
extern "C" bool verifyRollbackLater() { return true; }   // Arduino: we decide when it's good

void otaRollbackTick() {
    static bool settled = false;
    if (settled) return;
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!running || esp_ota_get_state_partition(running, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
        settled = true;      // flashed over USB, or already marked good
        return;
    }
    uint32_t up = millis();
    bool ok = meshCfg.home ? up > 60000 : meshHomeHeard();
    if (ok) {
        esp_ota_mark_app_valid_cancel_rollback();
        settled = true;
        if (isSerialConnected()) Serial.println("[OTA] new firmware confirmed (mesh is up)");
    } else if (up > 180000) {
        if (isSerialConnected()) Serial.println("[OTA] new firmware never reached the mesh: rolling back");
        delay(100);
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}
