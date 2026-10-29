// No-network mode: an OUI-SPY node on the Tab5's USB-A port is the data source. Its serial
// lines (TLM, detection JSON, RST status) are turned into the same JSON the dashboard sends
// (fleet / det / sighting), so the model and UI don't care where the data came from.
#pragma once
#include <Arduino.h>

void feed_start();
bool usb_live();          // a node is plugged in and talking
