// GPS on the Tab5's Grove port A (G53/G54): any M5Stack GPS unit (Unit GPS, Mini GPS/BDS,
// GNSS...) or other NMEA receiver. It finds the module by itself: both pin orders and the
// usual baud rates are tried until real NMEA sentences (valid checksums) come in, and it
// searches again if the module goes quiet. RMC + GGA give the fix.
#pragma once
#include <Arduino.h>

struct GpsFix {
    bool valid;            // a 2D/3D fix newer than 3 s
    double lat, lon;
    float alt;             // m above sea level
    float speed;           // m/s
    float course;          // deg, valid while moving
    float hdop;
    float acc;             // m, rough 68% radius (hdop x 5)
    uint8_t sats;
    uint32_t ms;           // millis() of the fix
};

void gps_begin();
GpsFix gps_fix();          // latest (valid false when none or stale)
const char *gps_state();   // "no GPS", "GPS 9600: 4 sats, no fix", "GPS 3D 9 sats"
bool gps_present();        // a module is talking

// Where the Tab5 has been (the map's own trail): up to GPS_TRAIL points, one every
// 3 m of movement.
constexpr int GPS_TRAIL = 600;
int gps_trail(float (*out)[2], int max);
