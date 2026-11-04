# What the firmware needs for the app to do everything

The app reads and drives boards over the console lines they already print. Most of the
mesh works today over USB. The rest needs these additions. Each item names the board, the
firmware, and the exact lines or verbs the app is written to use.

## Every board: a BLE line link (iOS, and Android without a cable)

- A GATT service carrying the console both ways, Nordic UART style:
  - one write characteristic for lines in;
  - one notify characteristic for lines out, `\n`-terminated, chunked to the MTU.
- The same lines as USB, so the app's engine (`lib/data/local_mesh.dart`) runs unchanged on top.
- Pairing: bonded, with a PIN shown on the board's screen or LED blinks. Never open.
- Firmware: `src/` (OUI-SPY detector mesh) and Recon Blue
  (`oui-spy-recon-blue/lib/oui_mesh`). Its radio time is shared with the BLE scan.

## OUI-SPY Recon (`oui-spy-recon-development/firmware/oui-spy-recon-blue`, the display S3)

1. **USB feed always on** (today a screen-only setting), or a console verb `feed always|mesh`.
   With the mesh off, nothing reaches USB.
2. **A read-only mode query**, e.g. `mode` → `OK mode <id> <state>`. Today only TLM carries it,
   and a bare `blue` reboots into no mode.
3. **Detector filters over the console:** `mfbegin` / `mfadd` / `mfdel` / `mfdata` / `mfend`
   and `preset <name>`, as `src/main.cpp` takes them, applied to the running detector's list.
   Today the list is set only through the Wi-Fi portal (`/save`, `/api/presets/apply`).
4. **FPV (the C5 receiver):**
   - Pass the link's ROW + INFO packets through to USB, framed so they don't mix with the
     text console. For example, a `FPV1` line announcing a binary block, or a separate
     USB CDC interface.
   - Take FPV verbs and turn them into link CMD packets:
     `fpv chan R6|<index>`, `fpv scan`, `fpv survey`, `fpv hop`, `fpv stop`,
     `fpv mode auto|fine|raw`, `fpv smooth 0|1`, `fpv stream 0|1`.
   - Print an FPV detection when it locks, and once a second while locked:
     `FPV {"mhz":5843,"ch":"R6","lock":1,"power":..,"gain":..,"clip":..,"video":1,"node":"XXXXXX"}`.
     Also flood it over the mesh like RID, so the feed, alerts and other nodes see it.
   - Bandwidth: AUTO is ~360-400 KB/s for 224x168 at ~24 fps, fine over native USB.
     BLE gets INFO and snapshots only.
5. **Wardrive mode using both chips:**
   - the C5 does 2.4 + 5 GHz Wi-Fi, BLE and 802.15.4 (Zigbee / Thread);
   - the S3 adds BLE.
   - Same `WD {json}` lines as `src/wardrive.cpp`, plus `"band":"2.4"|"5"` on Wi-Fi rows and
     `"t":"zigbee"` rows (PAN ID, extended PAN, channel 11-26, short/long address, RSSI, frame type).
   - Channel plans per node: `wd ch <plan>`; for 5 GHz, plans such as `36-48`, `149-165`, `all5`.
6. **Grove port: identify M5Stack units.**
   - At boot, and on `grove scan`, scan the port: an I2C address scan, and UART probes for
     GPS (NMEA) and LoRa / Meshtastic.
   - Print `GROVE {"bus":"i2c","addr":"0x44","unit":"ENV III","caps":["temp","hum"]}`.
   - Stream readings: `GRV {"unit":"ENV III","temp":21.4,"hum":40}`.
   - Take per-unit settings: `grove use <unit> log|alert <field> <op> <value>|gps|mesh`.
   - Address table to start from:

     | Address | Unit |
     |---|---|
     | 0x44 | SHT3x (ENV III / ENV II) |
     | 0x70 | QMP6988 (ENV III) |
     | 0x29 | VL53L0X / VL53L1X (ToF) |
     | 0x68 | MPU6886 / BMI270 (IMU) |
     | 0x76 / 0x77 | BMP280 / BME280 |
     | 0x5A | MLX90614 (NCIR) |
     | 0x23 | BH1750 (light) |
     | 0x62 | SCD40 (CO2) |
     | 0x38 | AHT20 |
     | 0x48 | ADS1100 / ADS1115 |
     | 0x3C | OLED |

     Confirm each by its ID register, not just its address.

## OUI-SPY detector mesh (`src/`)

- **BLE line link** (above).
- **Grove units** (as above) on boards with a Grove port.
- **FPV detection flood:** take `FPV` packets from a Recon and relay them like RID, so a plain
  OUI-SPY home carries them to the dashboard and the app.

## Unified Blue modes (current and legacy, `oui-spy-unified-blue`, Recon Blue)

AP-only settings that need console verbs. The app already parses every mode's lines.

| Mode | Needs |
|---|---|
| Detector | filter lists (ouis / macs), presets, buzzer / LED, aliases, lock: the `mf*` and `preset` verbs above, plus `audio <bz> <led>` |
| Foxhunter | `CMD:TARGET <mac>`, `CMD:AUDIO <bz> <led>` |
| PCAP / BLE Sniff | `CMD:FTMASK <hex>`, `CMD:FILTER bssid|oui <list>`, `CMD:RECORD start|pause|resume|stop`, and the capture as chunked lines (e.g. `PCAPB <seq> <base64>`), so the .pcap reaches the phone without the AP |
| Flock-You | `{"cmd":"dump_session"[,"prev"]}` ported from the standalone flock-you |
| Legacy wrapper | `CMD:MODE <n>` (select a mode over USB), `CMD:BUZZER 0|1` |
| Sky Spy, AXEOFF | `CMD:STATUS` / `CMD:VERSION` so they can be recognised by asking |

## The home dashboard (`home-dashboard/`)

- `GET /api/mobile/latest` → `{version, build, url, sha256}`, serving the newest APK, so phones
  update from it (`lib/data/updater.dart`).
- Fleet `online` aged when no source is feeding (nodes stay "online" with ages in the hours).
- Don't put the mobile user agent (`OUI-SPY-Mesh-Mobile` / `OUI-SPY-Recon`) in the "web" room if
  console events should stay browser-only. The app now uses them on purpose, for its console view.
