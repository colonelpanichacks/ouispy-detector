#pragma once
// Firmware updates over the ESP-NOW mesh (mesh_ota.cpp).
//
// The home stages an image (from the dashboard, over USB) in its spare OTA
// partition, then sends it to one remote at a time:
//   1. OFFER: a fleet-key-signed command (MESH_CMD_OTA_OFFER, wrapped in
//      MESH_CMD_TARGET) with the image size and SHA-256. Signing, replay
//      protection and addressing come from the command layer.
//   2. DATA: the image in 192-byte chunks as MESH_T_OTA frames, sealed with
//      the fleet key like every other frame, in windows of 32 chunks.
//   3. STATE: the target reports the window it is on and which chunks of it
//      arrived; the home resends only the missing ones.
// The target writes each completed window to its spare partition, checks the
// SHA-256 of everything it received against the signed offer, lets ESP-IDF
// validate the image, then boots it. The new firmware only marks itself good
// once it has heard the home over the mesh (or, on the home, after a minute
// of uptime); otherwise it restarts and the bootloader rolls back.
#include <Arduino.h>

#define MESH_CMD_OTA_OFFER 13   // payload: see mesh_ota.cpp otaOfferPayload

// Mesh hooks (mesh.cpp)
void meshOtaRx(const uint8_t* body, uint8_t len, const uint8_t* originMac, const uint8_t* rxHop);
// Command hook (main.cpp meshApplyCommand)
bool meshOtaOffer(const uint8_t* payload, uint16_t len);

// Home side, driven from the USB console (main.cpp)
bool otaStageBegin(uint32_t size, const uint8_t sha[32], String& err);
bool otaStageData(uint32_t offset, const uint8_t* data, uint16_t len, String& err);
bool otaStageEnd(String& err);
bool otaStaged(uint32_t& size);
// Builds the signed offer payload for a target; the caller sends it.
uint16_t otaOfferPayload(const uint8_t target[3], uint8_t* out);
void otaSendBegin(const uint8_t target[3], uint16_t session);
void otaSendAbort();
bool otaOffering();          // offer sent, target hasn't answered yet
bool otaSelfInstall(String& err);
void otaPrintStatus();

// Both sides
void meshOtaTick();
bool meshOtaBusy();          // sending or receiving: keep the radio on the mesh
void otaRollbackTick();      // mark a freshly updated image good, or roll back
