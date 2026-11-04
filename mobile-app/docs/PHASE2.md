# Phase 2: the app talks to a node directly (design sketch)

This is a sketch. Nothing in it is built yet. Phase 1, the dashboard client, is what ships in `lib/` today.

Phase 2 drops the Mac. The phone plugs into, or pairs with, one OUI-SPY board and does the dashboard's job itself:
- it is the mesh's **home**;
- it flashes firmware and switches a board between the single-purpose firmwares;
- it reads everything the Unified Blue modes produce;
- it controls every setting on the board.

## 1. Where it plugs in

The model and the views already sit behind `FeedSource` (`lib/data/dashboard.dart`). The dashboard is one implementation of it. Phase 2 adds two more, and both feed the same `Model.ingest*` calls:

```
            +-- DashboardSource   REST + Socket.IO   (phase 1)
Model  <----+-- SerialSource      USB OTG CDC         (Android)
            +-- BleSource         GATT, line service  (iOS + Android)
```

Both node sources carry the **same line feed the Tab5 parses over USB** (`tab5-dashboard/src/feed.cpp`). One Dart port of that parser serves both of them:

| Line | Meaning | Ingest |
|---|---|---|
| `{...,"mac":...}` / `DET {...}` | a detection, with `node` and `dom` | `ingestDet` (dedup 6 s, seen_by over 60 s, closest = strongest of 2+) |
| `TLM {...}` | every 5 s: identity, counters, roster (`meshNodesJson`) | `ingestFleet` |
| `RST {...}` | a node's settings and status (local `status` / `rstatus`) | node cfg |
| `RFL ...` | a node's filter list | filter editor |
| `CMD ...` / `ACK ...` | command queued / acknowledged | job states |
| `RID {...}` | Remote ID, local or relayed | `ingestDrone` |
| `WD {...}` | wardriver sighting | wardrive layer |
| `GPS {...}` | a GPS node's fix | node position / phone-less fix |

The host side of the feed matches the Tab5's:
- **Mesh view.** Like fleet.py, it computes `seen_by`, `closest`, dedup, and node online/offline (75 s for relayed nodes).
- **Device history.** A local SQLite (sqflite) holds the history, the same tables the dashboard keeps. The Devices HISTORY tab and the detail page then work with no Mac. The detail page's `rows` are built on the phone the same way `_flat_intel` builds them.
- **Tri fixes.** These need the RSSI model in `home-dashboard/tri.py`, ported to Dart (a small grid search).

## 2. Transports

### Android: USB OTG serial

- Use the `usb_serial` plugin (CDC-ACM). It covers the XIAO S3's USB-Serial/JTAG, VID 0x303A, at 115200.
- **Never toggle DTR or RTS on their own.** That pulses reset (see AGENTS.md). Open with both asserted in one control transfer, the way the Tab5 host does, or leave both untouched.
- Send a newline on open, and reopen after 12 s of silence. This recovers the CDC after an abrupt close, as the dashboard does.
- A foreground service (`flutter_foreground_task`, already used in nexus_companion) keeps the link and the alerts running with the screen off. It shows a notification while the phone is the home.

### iOS (and Android): BLE

iOS has no USB serial, so BLE is the only way in. That needs firmware work: a **GATT line service** on every image.

- Service `OUISPY-LINE`, with three characteristics:
  - `rx`: write without response. Console lines into the node.
  - `tx`: notify. The node's line feed out.
  - `info`: read. Firmware kind, version, board, capability JSON (section 5).
- MTU 247 with a length-prefixed framing, so long JSON lines span notifications.
- **Throttle the TX side.** BLE carries roughly 20 to 60 KB/s, well above the line rate, but a flood of detections must not starve the mesh. The node should batch, or drop repeat `upd` sightings before detections.
- **Security.**
  - LE Secure Connections with passkey pairing; the passkey shows on the node's display or comes from the fleet key.
  - The node accepts console writes only from a bonded phone.
  - The same fleet key gates commands, as it does on the mesh today.
- **Radio coexistence.** The S3's BLE shares the radio with ESP-NOW and the BLE scanner. The detector already parks the radio per mode (`meshParkChannel`, `meshNeedsAir`). A connected phone should be one more client of that arbitration: a long connection interval while scanning, a short one during a flash.

## 3. The phone as the mesh home

Today the home node, on USB to the Mac, is the bridge. With the phone in the Mac's place:

1. The board on the phone keeps its `home` role. The phone runs `command_sender`'s job queue (port `home-dashboard/commands.py`): `rstatus`, `raudio`, `rident`, `rpreset`, `rfilters`, `mfbegin`/`mfdata`/`mfend`, `meshrechan`, `meshpush`. It matches `CMD`/`ACK`/`RST`/`RFL` replies to jobs exactly as the dashboard does.
2. It also needs the fleet key, held in the iOS Keychain or the Android Keystore. To provision it, either scan the dashboard's QR (`/api/fleetkey/reveal` already exists for this) or generate it on the phone. The flasher generates it the same way: 32 alphanumeric characters from a CSPRNG. The key is never logged and never shown after setup.
3. **Hand-off.** When the phone is the home, the Mac is not, and vice versa: two homes on one fleet confuse the roster. The app shows which home is live and offers to take over.
4. **Back to the Mac.** When the phone returns to the LAN with a dashboard, it uploads its local history to the dashboard, so nothing heard away from home is lost. This needs a new endpoint, `POST /api/import` with the same rows as `/api/detections`.

## 4. Flashing firmware and switching firmwares

- **Android over USB.**
  - Port esptool's ROM protocol (SLIP, SYNC, FLASH_BEGIN/DATA/END, with MD5 verify). It is small and well documented, and esptool-js is a readable reference.
  - Enter the bootloader with the USB-Serial/JTAG reset sequence: DTR/RTS set together, in the order esptool uses.
  - Images come from a manifest like the web flasher's `manifest.json`. One entry per firmware: detector, sky-spy, wardriver, foxhunter, flock-you, Unified Blue, the Recon images. Each entry carries the image URL, SHA-256, board, and partition layout.
- **Where images come from.** Privacy first, so:
  - from the user's own dashboard (`/api/firmware/library` already lists them; downloads go through the dashboard), or
  - from images bundled into the app.

  The GitHub release URL is an explicit option the user turns on, never the default.
- **Over the air.**
  - Nodes on the two-slot partition table take an OTA image over the mesh. The dashboard already drives `fw_install`, `fw_stage` and `fw_send`. From the phone, the same jobs go out through its home node.
  - Over BLE, the phone streams the image straight to the connected node: `esp_ota_begin`/`write`/`end` behind the line service, with chunk CRCs. This is the same chunk-and-repair scheme the filter push uses.
- **Switching single firmwares.** This is a flash of a different image, with the node's identity kept: fleet key, alias, mesh role and channel.
  - Before the flash, the app reads them with `status`.
  - After the flash, it writes them back with `mesh`, `meshkey`, `nodealias` and `audio`. Those are the flasher's step 4 commands, already specified in the shared notes.
  - The NVS keys must be stable across firmwares for this to survive without the replay. Note that in the firmware design.

## 5. Every setting on the board, from the app

The user wants everything on the board tunable from the phone: the SD card, the IMU, vibration on and off, alerts on the lights, notifications.

Hard-coding a screen per board would rot. Each firmware should instead **describe its own settings**, and the app should render them.

- **`caps` command.** It returns JSON, answered locally and relayed over the mesh as a STATUS extension, or as a new `MESH_CMD_CAPS`. The JSON lists the hardware and every setting with its type, range, and current value:

  ```json
  {"fw":"unified-blue","ver":"1.4.0","board":"oui-spy-recon",
   "hw":["sd","imu","vibe","rgb","buzzer","gps","display"],
   "set":[
     {"k":"vibe.on","t":"bool","v":true,"label":"Vibration"},
     {"k":"vibe.pattern","t":"enum","v":"pulse","opts":["off","tick","pulse","sos"]},
     {"k":"led.alert","t":"enum","v":"flash","opts":["off","solid","flash","rainbow"]},
     {"k":"led.color","t":"hue","v":300},
     {"k":"led.bright","t":"int","v":40,"min":0,"max":255},
     {"k":"buzzer.on","t":"bool","v":false},
     {"k":"imu.wake","t":"bool","v":true,"label":"Wake on motion"},
     {"k":"imu.tamper","t":"bool","v":false,"label":"Alert when moved"},
     {"k":"sd.log","t":"enum","v":"detections","opts":["off","detections","pcap","all"]},
     {"k":"notify.min_rssi","t":"int","v":-90,"min":-110,"max":-30}
   ],
   "act":["identify","locate","sd.format","sd.eject","imu.calibrate","reboot"]}
  ```

- **`set <key> <value>` / `get <key>`.** Persist to NVS, reply `OK`/`ERR`. Remote nodes get the same over the mesh as a signed `MESH_CMD_SET`.
- **App side.** It draws one generic settings page from `set` and `act`:
  - bools become switches, enums become pills, ints become sliders, hues become a colour wheel.
  - Every new firmware feature appears in the app with no app update.
- **SD card.**
  - List, download and delete logs or pcaps: `sd.ls`, then `sd.get <path>` streamed over the line link with chunk CRCs.
  - Format and eject.
  - Show free space.
- **IMU.**
  - Live tilt and motion state.
  - Wake-on-motion.
  - A "this node was moved" tamper alert, which arrives as a detection-class event so it alerts like one.
- **Alerts on the board.**
  - Per alert class (detection, drone, tamper, node lost), choose vibration, LED pattern and colour, and buzzer. The class keys are named in `caps`.
  - The phone's own alerts (notification, vibrate, sound, silence) already exist in phase 1 Settings and on the Alerts page.

## 6. Unified Blue

Unified Blue runs one mode at a time: detector, foxhunter, flock-you, pcap, sky-spy, ble-sniff, axeoff. On the Recon board it runs inside the display shell (see the shared notes, "OUI SPY RECON BLUE"). The app needs:

- **Mode switching.** A `mode <name>` command, with the current mode in `caps`.
- **One line type per mode,** prefixed like the detector's. A new mode is a new prefix plus a view, and the parser stays one table.
  - `FOX {mac,rssi,...}` drives a foxhunt view: a big RSSI needle and a "hotter / colder" tone, with phone vibration as an option.
  - `FLOCK {...}` is a Flock camera sighting and joins the alert pipeline.
  - `SNIFF {...}` is a BLE advert, shown in a live table.
  - `PCAP` is status only; the capture goes to SD, and the files are pulled with `sd.get`.
  - Sky-spy already speaks `RID`.
- **The Recon C5's 5 GHz Remote ID** arrives through the display's link as `LINK_T_LINE` lines. The app sees the same `RID` lines.

## 7. Firmware work this needs (owners: whoever holds each firmware)

1. GATT line service + bonding on every image (S3: NimBLE; C5: its own BLE).
2. `caps`, `set`, `get`, `mode` console commands; `MESH_CMD_SET` / `MESH_CMD_CAPS` for remotes.
3. `sd.*`, `imu.*`, `vibe.*`, `led.*` setting keys where the board has the hardware.
4. Stable NVS keys for identity across all firmwares.
5. Line prefixes for each Unified Blue mode.
6. The two-slot partition migration (already on the AGENTS.md list) for OTA over mesh and BLE.

## 8. Order of work

1. Android USB serial source, reusing the Tab5 parser (no firmware change). The phone works as a local display for one node and its mesh.
2. Phone as home: job queue, fleet key, local history.
3. Android USB flashing from a manifest, with identity carried across firmwares.
4. Firmware: GATT line service. Then the iOS BLE source, with the same views.
5. `caps`/`set` firmware commands and the generic settings page; SD and IMU pages.
6. OTA over BLE and over the mesh from the phone.
7. Unified Blue mode views.
