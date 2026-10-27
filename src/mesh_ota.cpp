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
#define OTA_SUB_STATE  2       // target -> home: base(2) bitmap(4) status(1) err(1)
#define OTA_SUB_POLL   3       // home -> target: "where are you?"
#define OTA_SUB_ABORT  4       // home -> target
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
enum { TX_IDLE, TX_OFFER, TX_SEND, TX_DONE, TX_FAIL };
static uint8_t txState = TX_IDLE;
static uint8_t txTarget[3];
static uint16_t txSession;
static uint32_t txChunks, txBase, txAcked;       // window base; acked bitmap of it
static uint32_t txSentMask;                      // sent in the current round
static uint32_t txOfferAt, txLastStateAt, txLastPushAt, txPollAt, txStartAt, txReportAt;
static uint8_t txPolls;
static bool txDirect = false;                    // target heard first-hand
static char txErr[48];

static uint32_t winSize(uint32_t base) { uint32_t left = txChunks - base; return left < OTA_WIN ? left : OTA_WIN; }
static uint32_t winFull(uint32_t base) { uint32_t n = winSize(base); return n >= 32 ? 0xFFFFFFFFUL : ((1UL << n) - 1); }

static void txReport(const char* state) {
    if (!isSerialConnected()) return;
    char id[7];
    hexId(txTarget, id);
    uint32_t done = txBase * OTA_CHUNK;
    if (done > stageSize) done = stageSize;
    uint32_t pct = stageSize ? (uint32_t)((uint64_t)done * 100 / stageSize) : 0;
    uint32_t secs = (millis() - txStartAt) / 1000;
    uint32_t kbps = secs ? (done * 8 / 1000) / secs : 0;
    Serial.printf("OTA {\"node\":\"%s\",\"state\":\"%s\",\"pct\":%u,\"bytes\":%u,\"size\":%u,\"kbps\":%u,\"direct\":%d,\"err\":\"%s\"}\n",
                  id, state, pct, done, stageSize, kbps, txDirect ? 1 : 0, txErr);
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
    // {session u16, size u32, sha[32], home id[3]}
    uint8_t home[3];
    meshSelfIdBytes(home);
    uint16_t session = (uint16_t)random(1, 0x10000);
    put16(out, session);
    put32(out + 2, stageSize);
    memcpy(out + 6, stageSha, 32);
    memcpy(out + 38, home, 3);
    return 41;
}

void otaSendBegin(const uint8_t target[3], uint16_t session) {
    memcpy(txTarget, target, 3);
    txSession = session;
    txChunks = (stageSize + OTA_CHUNK - 1) / OTA_CHUNK;
    txBase = 0;
    txAcked = 0;
    txSentMask = 0;
    txPolls = 0;
    txDirect = false;
    txErr[0] = 0;
    txOfferAt = txStartAt = txReportAt = millis();
    txLastStateAt = txLastPushAt = txPollAt = 0;
    txState = TX_OFFER;
    txReport("offered");
}

void otaSendAbort() {
    if (txState == TX_OFFER || txState == TX_SEND) txFail("aborted");
}

static void txOnState(const uint8_t* b, uint8_t len, const uint8_t* originMac, const uint8_t* rxHop) {
    if (txState != TX_OFFER && txState != TX_SEND) return;
    if (memcmp(b, txTarget, 3) != 0 || get16(b + 4) != txSession || len < OTA_HDR + 8) return;
    if (memcmp(originMac, rxHop, 6) == 0) txDirect = true;
    uint32_t base = get16(b + OTA_HDR), bitmap = get32(b + OTA_HDR + 2);
    uint8_t status = b[OTA_HDR + 6], err = b[OTA_HDR + 7];
    txLastStateAt = millis();
    txPolls = 0;
    if (status == OTA_ST_FAIL) { txFail(err < sizeof(OTA_ERR) / sizeof(OTA_ERR[0]) ? OTA_ERR[err] : "target failed"); return; }
    if (status == OTA_ST_DONE) { txBase = txChunks; txState = TX_DONE; txReport("done"); return; }
    if (txState == TX_OFFER) txState = TX_SEND;
    if (base > txBase) { txBase = base; txAcked = bitmap; txSentMask = 0; }
    else if (base == txBase) { txAcked = bitmap; txSentMask = txAcked; }   // resend only what's missing
}

static void txTick(uint32_t now) {
    if (txState == TX_OFFER) {
        meshHoldRadio(2000);
        if (now - txOfferAt > 45000) txFail("target never answered the offer");
        return;
    }
    if (txState != TX_SEND) return;
    meshHoldRadio(2000);
    if (now - txReportAt > 2000) { txReportAt = now; txReport("sending"); }
    uint32_t full = winFull(txBase);
    uint32_t need = full & ~txAcked & ~txSentMask;
    // Push the window's missing chunks while the TX queue has room (two
    // slots stay free for announces, acks and relays).
    while (need && meshTxFree() > 2) {
        uint32_t i = __builtin_ctz(need);
        uint32_t idx = txBase + i;
        uint8_t b[OTA_HDR + 2 + OTA_CHUNK];
        otaHeader(b, OTA_SUB_DATA, txSession, txTarget);
        put16(b + OTA_HDR, (uint16_t)idx);
        uint32_t off = idx * OTA_CHUNK;
        uint16_t n = (off + OTA_CHUNK <= stageSize) ? OTA_CHUNK : (uint16_t)(stageSize - off);
        if (esp_partition_read(stagePart, off, b + OTA_HDR + 2, n) != ESP_OK) { txFail("staged image unreadable"); return; }
        if (!meshOtaSend(b, OTA_HDR + 2 + n, !txDirect)) break;
        txSentMask |= 1UL << i;
        need &= ~(1UL << i);
        txLastPushAt = now;
    }
    // Whole window out and no word from the target: ask where it is.
    if (!need && now - txLastPushAt > 700 && now - txLastStateAt > 700 && now - txPollAt > 700) {
        if (++txPolls > 40) { txFail("target went silent"); return; }
        uint8_t b[OTA_HDR];
        otaHeader(b, OTA_SUB_POLL, txSession, txTarget);
        meshOtaSend(b, sizeof(b), !txDirect || txPolls > 3);   // after a few misses, try relays too
        txPollAt = now;
        if (txPolls > 3) txDirect = false;
    }
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
// Target: receiving
// =====================================================================
static bool rxActive = false;
static uint16_t rxSession;
static uint8_t rxHome[3];
static uint32_t rxSize, rxChunks, rxBase, rxHave, rxLastAt, rxStateAt, rxDoneAt;
static uint8_t rxSha[32], rxStatus, rxErr;
static uint8_t rxWin[OTA_WIN * OTA_CHUNK];
static uint8_t* rxImg = nullptr;             // whole image in PSRAM; flash is written at the end
static esp_ota_handle_t rxHandle;
static const esp_partition_t* rxPart = nullptr;
static mbedtls_sha256_context rxCtx;
static bool rxDirty = false;   // got data since the last STATE

static void rxSendState() {
    uint8_t b[OTA_HDR + 8];
    otaHeader(b, OTA_SUB_STATE, rxSession, rxHome);
    put16(b + OTA_HDR, (uint16_t)rxBase);
    put32(b + OTA_HDR + 2, rxHave);
    b[OTA_HDR + 6] = rxStatus;
    b[OTA_HDR + 7] = rxErr;
    meshOtaSend(b, sizeof(b), true);
    rxStateAt = millis();
    rxDirty = false;
}

static void rxFinish(uint8_t err) {
    rxErr = err;
    rxStatus = err ? OTA_ST_FAIL : OTA_ST_DONE;
    if (rxImg) { free(rxImg); rxImg = nullptr; }
    if (err) {
        rxActive = false;
        if (isSerialConnected()) Serial.printf("[OTA] update failed: %s\n", OTA_ERR[err]);
    } else {
        rxDoneAt = millis();
        if (isSerialConnected()) Serial.println("[OTA] update verified, restarting into it");
    }
    mbedtls_sha256_free(&rxCtx);
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
    if (rxActive) mbedtls_sha256_free(&rxCtx);
    if (rxImg) { free(rxImg); rxImg = nullptr; }
    rxActive = false;
    rxPart = esp_ota_get_next_update_partition(nullptr);
    if (!rxPart || size == 0 || size > rxPart->size) return false;
    rxImg = (uint8_t*)ps_malloc(size);
    if (!rxImg) return false;
    rxSession = session;
    rxSize = size;
    rxChunks = (size + OTA_CHUNK - 1) / OTA_CHUNK;
    memcpy(rxSha, p + 6, 32);
    memcpy(rxHome, p + 38, 3);
    rxBase = 0;
    rxHave = 0;
    rxStatus = OTA_ST_RECV;
    rxErr = OTA_E_NONE;
    rxDoneAt = 0;
    mbedtls_sha256_init(&rxCtx);
    mbedtls_sha256_starts(&rxCtx, 0);
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
    if (idx < rxBase || idx >= rxBase + OTA_WIN || idx >= rxChunks) return;
    uint32_t bit = 1UL << (idx - rxBase);
    uint16_t n = len - (OTA_HDR + 2);
    uint32_t expect = (idx == rxChunks - 1) ? rxSize - idx * OTA_CHUNK : OTA_CHUNK;
    if (n != expect) return;
    if (!(rxHave & bit)) {
        memcpy(rxWin + (idx - rxBase) * OTA_CHUNK, b + OTA_HDR + 2, n);
        rxHave |= bit;
        rxDirty = true;
    }
    uint32_t left = rxChunks - rxBase;
    uint32_t full = left >= 32 ? 0xFFFFFFFFUL : ((1UL << left) - 1);
    if ((rxHave & full) != full) return;
    // Window complete: write it, hash it, move on.
    uint32_t bytes = (left >= OTA_WIN ? OTA_WIN * OTA_CHUNK : rxSize - rxBase * OTA_CHUNK);
    memcpy(rxImg + rxBase * OTA_CHUNK, rxWin, bytes);
    mbedtls_sha256_update(&rxCtx, rxWin, bytes);
    rxBase += left >= OTA_WIN ? OTA_WIN : left;
    rxHave = 0;
    if (rxBase >= rxChunks) {
        uint8_t got[32];
        mbedtls_sha256_finish(&rxCtx, got);
        if (memcmp(got, rxSha, 32) != 0) { rxFinish(OTA_E_HASH); return; }
        // Verified in RAM; now the flash, in one pass with the radio idle.
        if (esp_ota_begin(rxPart, rxSize, &rxHandle) != ESP_OK) { rxFinish(OTA_E_WRITE); return; }
        for (uint32_t off = 0; off < rxSize; off += 4096) {
            uint32_t n = rxSize - off < 4096 ? rxSize - off : 4096;
            if (esp_ota_write(rxHandle, rxImg + off, n) != ESP_OK) { esp_ota_abort(rxHandle); rxFinish(OTA_E_WRITE); return; }
        }
        if (esp_ota_end(rxHandle) != ESP_OK) { rxActive = false; rxFinish(OTA_E_IMAGE); return; }
        if (esp_ota_set_boot_partition(rxPart) != ESP_OK) { rxActive = false; rxFinish(OTA_E_BOOT); return; }
        rxFinish(OTA_E_NONE);
        return;
    }
    rxSendState();
}

void meshOtaRx(const uint8_t* b, uint8_t len, const uint8_t* originMac, const uint8_t* rxHop) {
    if (len < OTA_HDR) return;
    switch (b[3]) {
        case OTA_SUB_DATA:  rxOnData(b, len); break;
        case OTA_SUB_POLL:  if (rxActive && get16(b + 4) == rxSession) rxSendState(); break;
        case OTA_SUB_ABORT:
            if (rxActive && get16(b + 4) == rxSession) {
                rxActive = false;
                mbedtls_sha256_free(&rxCtx);
                if (rxImg) { free(rxImg); rxImg = nullptr; }
            }
            break;
        case OTA_SUB_STATE: txOnState(b, len, originMac, rxHop); break;
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
    // A partial window and a lull in the data: say what's missing.
    if (rxDirty && now - rxLastAt > 350 && now - rxStateAt > 350) rxSendState();
    if (now - rxLastAt > 90000) {
        mbedtls_sha256_free(&rxCtx);
        if (rxImg) { free(rxImg); rxImg = nullptr; }
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
