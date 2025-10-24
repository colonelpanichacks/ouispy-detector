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

### Device Management
- **Device Aliasing:** Assign custom names to detected devices
- **Persistent History:** All detected devices saved to NVS (up to 100 devices)
- **Automatic Sync:** Device list updates across reboots
- **Clear History:** Remove all stored device records

### Burn In Settings
- **Permanent Lock:** Lock configuration and bypass setup on boot
- **Instant Scanning:** Device boots directly into scanning mode
- **Protected Settings:** All filters, aliases, and preferences preserved
- **Secure Reset:** Requires flash erase + reflash to unlock

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

### Device Alias Management
Assign custom names to detected devices via the web portal:

1. **Access Portal:** Connect to device AP and navigate to `http://192.168.4.1`
2. **View Devices:** Detected devices appear in "Device Alias Management" section
3. **Set Alias:** Enter custom name next to any device and click "Set Alias"
4. **Remove Alias:** Clear the name field and click "Set Alias" to remove
5. **Clear History:** Use "Clear Device History" button to remove all stored devices

**Storage:** Up to 100 devices stored in NVS, persists across reboots and power cycles.

### Burn In Configuration
Permanently lock settings for deployment scenarios:

#### What Gets Locked
- All OUI/MAC filters and descriptions
- Device aliases and detection history
- Buzzer and LED preferences
- Configuration window disabled
- WiFi AP disabled

#### How to Lock
1. Configure all desired filters and aliases
2. Navigate to "Burn In Settings" section
3. Read warnings carefully
4. Click "Lock Configuration Permanently"
5. Confirm three separate prompts
6. Device restarts in 3 seconds into scanning mode

#### How to Unlock
**Required:** Flash erase followed by firmware reflash
```bash
# Erase flash memory
pio run -e seeed_xiao_esp32s3 --target erase

# Reflash firmware
pio run -e seeed_xiao_esp32s3 --target upload
```

**Warning:** Simple reflash without erase will NOT unlock the device.

## Operation

### Startup Sequence
1. MAC randomization (stealth mode)
2. Load saved configuration, aliases, and device history
3. Configuration mode (20-second timeout) *OR* direct to scanning if burned in
4. BLE scanning activation
5. Target detection and audio alerts
6. Auto-save device history every 60 seconds

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

Loading configuration...
Device aliases loaded from NVS (3 aliases)
Detected devices loaded from NVS (15 devices)

=== STARTING SCANNING MODE ===
Configured Filters:
- AA:BB:CC (OUI) - "DJI Drones"
- AA:BB:CC:12:34:56 (MAC) - "Test Device"

>> Match found! <<
Device: AA:BB:CC:ab:cd:ef (My Drone) | RSSI: -45
Filter matched: DJI Drones (OUI)

Device aliases saved to NVS (3 aliases)
Detected devices saved to NVS (16 devices)
```

## Troubleshooting

**No WiFi AP:** Wait 30 seconds after power-on, or device may be burned in (requires flash erase)
**No web portal:** Ensure connected to `snoopuntothem`, disable mobile data
**No audio:** Check buzzer connection (GPIO3)
**No LED:** Check NeoPixel wiring (GPIO4, 3.3V, GND)
**No detection:** Verify target device is advertising BLE
**Aliases not saving:** Check NVS storage space, maximum 100 devices/aliases
**Device history empty:** Devices only appear after detection during a scanning session
**Can't unlock burned config:** Must erase flash first, then reflash firmware

## Technical Specifications

- **Platform:** ESP32-S3
- **Scan interval:** 3 seconds
- **Range:** 10-30 meters (typical)
- **Storage:** NVS flash memory (filters, aliases, device history)
- **Device history:** Up to 100 devices with persistent storage
- **Processing:** Dual-core optimization
- **Audio:** GPIO3 buzzer with PWM control
- **Visual:** GPIO4 NeoPixel with synchronized animations
- **Auto-save:** Device data saved every 60 seconds during scanning

## License

Open source project. Modifications welcome. 
