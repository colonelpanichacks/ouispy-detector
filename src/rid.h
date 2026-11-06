// Drone Remote ID (ASTM F3411 / OpenDroneID) on every OUI-SPY node: the same
// decoding Sky-Spy does (WiFi beacon vendor IE, WiFi NAN action frames, BLE
// legacy adverts), merged per drone, printed as "RID {json}" and flooded over
// the mesh (MESH_T_DRONE) so the home hears every node's drones.
#pragma once
#include <Arduino.h>

#define RID_BODY_LEN 109   // mesh body after the 3-byte node id is included
// The rest of what Remote ID says (height reference, accuracies, fix time, EU class,
// operating area, a second ID, authentication), after the 109 bytes. It fills the same
// sealed frame size (192), so it costs no airtime; a node with older firmware reads the
// first 109 bytes and never sees it.
#define RID_EXT_LEN  44
#define RID_WIRE_LEN (RID_BODY_LEN + RID_EXT_LEN)

// Producers (radio callbacks; heap-free).
void ridWifiFrame(const uint8_t* frame, int len, int8_t rssi);
void ridBleAdvert(const uint8_t* adv, int len, const uint8_t mac[6], int8_t rssi);

// loop(): rate-limited reports of changed drones (serial + mesh), expiry.
void ridLoop();

// Mesh: a drone report from another node (body = after the packet header).
void ridFromMesh(const uint8_t* body, int len);

extern uint32_t ridLastSendMs;   // millis() of the last drone report queued for the mesh

// Number of drones heard in the last minute (telemetry).
uint8_t ridActive();
