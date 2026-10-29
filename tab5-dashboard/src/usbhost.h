// USB host on the Tab5's USB-A port: reads an OUI-SPY node's USB serial (ESP32 USB-Serial-JTAG,
// a CDC-ACM data interface) so the Tab5 can run with no network at all.
#pragma once
#include <Arduino.h>

void usb_start();
bool usb_attached();
const char *usb_device();            // "303A:1001" etc., "" when nothing is plugged in
size_t usb_read(char *buf, size_t max, uint32_t waitMs);   // bytes from the node
bool usb_send(const char *line);     // one console line, "\n" appended
