// Mesh link: a Meshtastic radio on Grove port A (M5Stack Unit C6L, serial module in TEXTMSG
// mode, on the private ouispymesh channel) keeps a Tab5 out of Wi-Fi range in touch with the
// home: the dashboard's Meshtastic radio sends a summary of the mesh every minute, and this end
// sends its position and what its nodes detect: two ESP-NOW meshes, one LoRa link between.
// Coded lines, one per text message (home-dashboard/mtastic.py has the other end):
//   in   OSM1 S <on>/<all> <d24> <id>:<up>:<rssi>,...   the mesh, from home
//        OUI-SPY ...                                     the home's alerts, shown as toasts
//   both OSM2 B<base64>                                  detections, up to 12 a message
//   out  OSM1 P <node> <lat> <lon> <acc>                 where we are
//        OSM1 W <node> <wifi> <ble>                      a wardriver's session counts
//        OSM1 H <node> wifi|lora                         hello: on Wi-Fi (home sends nothing) or not
#pragma once
#include <Arduino.h>

void meshlink_run();   // gps task, while S.meshLink: owns Serial1 on port A, returns when it's off
void meshlink_det(const char *node, const char *mac, int rssi, bool wifi, bool isNew);   // our nodes' detections, to home
const char *meshlink_state();   // for Settings and /info
void meshlink_info(char *out, size_t n);   // /info "link": what came in and went out
