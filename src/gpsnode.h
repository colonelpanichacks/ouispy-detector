// AtomS3 on the Atomic GPS Base (GPS on G5 at 9600 baud; the microSD slot is unused), in
// two builds, both full mesh nodes:
//   atomgps     GPS node: a detector that knows where it is.
//   atomgps-wd  GPS wardriver: every sighting stamped with its own fix (the dashboard
//               keeps them and exports WiGLE CSV).
// Both send their position over the mesh (MESH_T_POS) so they move on the maps, and on a
// Tab5's USB they are that Tab5's GPS ("GPS {...}" lines). Tap the screen: satellites /
// node. It is edged green while locked, red while not.
#pragma once
#include <Arduino.h>

void gpsNodeBegin();   // setup(), before scanning starts
void gpsNodeLoop();    // loop(): NMEA, screen, card, position on the mesh and USB

// The current fix (false without one, or when it is older than 3 s).
bool gpsNodeFix(double& lat, double& lon, float& acc);
