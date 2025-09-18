# OUI-SPY - Detector

![OUI-SPY](ouispy.png)

Professional BLE scanning system that detects specific devices by MAC address or OUI with audio feedback.

## Hardware

**OUI-SPY Board** - Available on [Tindie](https://www.tindie.com)
- ESP32-S3 based detection system
- Integrated buzzer and power management
- Ready-to-use, no additional components required

**Alternative:** Standard ESP32-S3 with external buzzer on GPIO3

**Enhanced Version:** Same firmware, add NeoPixel LED for visual feedback
- Buzzer: GPIO3 (D2)
- NeoPixel: GPIO4 (D3)
- **Note:** One firmware supports both configurations automatically

## Quick Start

1. **Power on device** - Creates WiFi AP `snoopuntothem` (password: `astheysnoopuntous`)
2. **Connect and configure** - Navigate to `http://192.168.4.1`
3. **Add targets** - Enter OUI prefixes (`AA:BB:CC`) or full MAC addresses
4. **Save configuration** - Device automatically switches to scanning mode

## Features

### Detection System
- OUI filtering for device manufacturers
- Full MAC address matching
- Persistent configuration storage
- Automatic timeout handling

### Audio & Visual Feedback
- **Audio:** 2 ascending beeps (ready), 3 beeps (detection), 2 beeps (re-detection)
- **Visual:** Pink breathing LED during scanning, blue-pink-purple flash on detection
- **Synchronized:** LED flashes match beep timing perfectly
- Smart cooldown prevents spam

### Privacy
- MAC address randomization on boot
- Stealth mode operation
- No traceable hardware fingerprints

## Installation

### PlatformIO
```bash
cd ouibuzzer-main/ouibuzzer
python3 -m platformio run --target upload
```

### Dependencies
- NimBLE-Arduino ^1.4.0
- ESP Async WebServer ^3.0.6
- Preferences ^2.0.0
- Adafruit NeoPixel ^1.12.0 (for LED functionality)

## Configuration

### Web Portal
Access via `http://192.168.4.1` after connecting to `snoopuntothem` AP:

**OUI Prefixes:** `AA:BB:CC` (matches specific manufacturers)
**MAC Addresses:** `AA:BB:CC:12:34:56` (specific devices)

Multiple entries supported (one per line).

## NeoPixel Wiring (Optional Enhancement)

### Hardware Requirements
- Adafruit NeoPixel (WS2812B) or compatible LED
- ESP32-S3 board (Seeed Xiao ESP32-S3 recommended)
- **Same firmware works with or without NeoPixel**

### Wiring Diagram
```
ESP32-S3 Xiao    →    NeoPixel
─────────────────────────────────
GPIO4 (D3)       →    Data Input (DIN)
3.3V             →    VCC (Power)
GND              →    GND (Ground)
```

### LED Behavior
- **Normal Scanning:** Pink breathing animation (smooth brightness fade)
- **Detection:** Blue → Pink → Purple → Blue flash sequence
- **Synchronization:** LED flashes perfectly match buzzer beep timing
- **Brightness:** Normal (50/255), Detection (200/255)

### Filter Types
- **OUI:** First 3 bytes (manufacturer prefix)
- **MAC:** Complete 6-byte address
- **Format:** Supports colons, hyphens, or spaces

## Operation

### Startup Sequence
1. MAC randomization (stealth mode)
2. Configuration mode (20-second timeout)
3. BLE scanning activation
4. Target detection and audio alerts

### Detection Logic
- Continuous BLE scanning
- Real-time MAC/OUI matching
- Cooldown system prevents duplicate alerts
- Memory management for device tracking

## Serial Output

```
=== OUI Spy Enhanced BLE Detector ===
Original MAC: d8:3b:da:45:aa:a0
Randomized MAC: a2:f3:91:7e:8c:45

=== STARTING SCANNING MODE ===
Configured Filters:
- AA:BB:CC (OUI)
- AA:BB:CC:12:34:56 (MAC)

>> Match found! <<
Device: AA:BB:CC:ab:cd:ef | RSSI: -45
Filter matched: OUI
```

## Troubleshooting

**No WiFi AP:** Wait 30 seconds after power-on
**No web portal:** Ensure connected to `snoopuntothem`, disable mobile data
**No audio:** Check buzzer connection (GPIO3)
**No LED:** Check NeoPixel wiring (GPIO4, 3.3V, GND)
**No detection:** Verify target device is advertising BLE

## Technical Specifications

- **Platform:** ESP32-S3
- **Scan interval:** 3 seconds
- **Range:** 10-30 meters (typical)
- **Storage:** NVS flash memory
- **Processing:** Dual-core optimization
- **Audio:** GPIO3 buzzer with PWM control
- **Visual:** GPIO4 NeoPixel with synchronized animations

## License

Open source project. Modifications welcome. 
