#pragma once
#include <Arduino.h>

void net_start();               // spawns the network tasks
String net_ip();
int net_rssi();
String net_target();            // "host:port" as resolved
bool net_ota_enabled();
bool net_online();                       // talking to the dashboard (not USB-only)
void net_bulk_begin();                   // around any big HTTP download (see net.cpp)
void net_bulk_end();
String net_base();                       // "http://ip:port" of the dashboard, or ""
bool net_post(const char *path, const char *body);   // queue a JSON POST to the dashboard
// Wi-Fi scan for the Settings screen (runs on the net task; results read under model lock).
void net_request_scan();
int net_scan_results(String *out, int *rssi, int max);
void net_bridge_line(const char *line);   // forward a USB node line to the dashboard (when bridging)
