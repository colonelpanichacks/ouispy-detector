# OUI-SPY Mesh

A local web dashboard for the OUI-SPY Detector **home node**. It shows the whole
mesh fleet live, puts the nodes on a map, and keeps a searchable history of every
detection. It runs entirely on your machine: no cloud, no accounts, no telemetry.

## Run

Works the same on Linux, macOS and Windows: Python 3.9+ and a terminal. Run
it from inside `home-dashboard/`.

Linux / macOS:

```bash
cd home-dashboard
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python ouispymesh.py
```

Windows (PowerShell or Windows Terminal):

```powershell
cd home-dashboard
py -m venv .venv
.venv\Scripts\pip install -r requirements.txt
.venv\Scripts\python ouispymesh.py
```

Then open http://127.0.0.1:5077. The terminal also prints the LAN and
Tailscale links.

| Option | What it does |
|---|---|
| *(none)* | Live mode: auto-detects the home node on USB (Espressif VID `0x303A`), plus MQTT if set up in Settings |
| `--demo` | Simulated 5-node fleet with 24 h of backfilled history. Needs no hardware and uses its own database |
| `--serial PORT` | A specific serial port: `COM5` (Windows), `/dev/ttyACM0` (Linux), `/dev/cu.usbmodem1101` (macOS). `--serial off` turns serial off |
| `--mqtt 192.168.1.10 --topic ouispy/detection` | Subscribe to the home node's MQTT detections |
| `--record capture.log` | Also append every raw serial line (with a timestamp) to a file |
| `--replay capture.log --speed 4` | Replay a recorded capture through the dashboard |
| `--host` / `--port` | Bind address and port (default all interfaces, port 5077). `--host 127.0.0.1` restricts it to this computer |

**Linux serial permissions:** add yourself to the serial group once with
`sudo usermod -aG dialout $USER` (`uucp` on Arch), then log out and back in.

## Start automatically

Ready-made files are in [`autostart/`](autostart/). Edit the paths in them to
match where you put the repo.

- **macOS (launchd):** copy `autostart/com.ouispy.mesh.plist` to
  `~/Library/LaunchAgents/`, then run
  `launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.ouispy.mesh.plist`.
  Run `launchctl kickstart -k gui/$(id -u)/com.ouispy.mesh` to restart it after
  updating, and `launchctl bootout gui/$(id -u)/com.ouispy.mesh` to stop it
  (stop it before flashing the home over USB).
- **Linux (systemd, per user):** copy `autostart/ouispymesh.service` to
  `~/.config/systemd/user/`, then run
  `systemctl --user daemon-reload && systemctl --user enable --now ouispymesh`.
  `loginctl enable-linger $USER` keeps it running without a login session.
  Run `systemctl --user stop ouispymesh` before flashing.
- **Windows (Task Scheduler):** run `autostart\install-windows-task.ps1` once in
  PowerShell. It starts OUI-SPY Mesh at logon and restarts it if it stops.
  Run `Stop-ScheduledTask OUISPYMesh` before flashing.

## Data path

In burned-in scanning mode the home node has no web server, so the dashboard reads
it two ways:

1. **USB serial (recommended).** Firmware with telemetry prints one line every 5 s:

   ```
   TLM {"v":1,"id":"A1B2C3","role":"home","mode":"scan","mesh":1,"ch":6,"up":3600,
        "heap":151000,"dev":4,"filters":12,"n":3,"rx":..,"q":..,"dup":..,"self":..,"bad":..,
        "tx":..,"txf":..,"txcb":..,"pend":0,"lrssi":-61,"heal":0,"mqtt":1,
        "nodes":[{"id":"D4E5F6","role":"remote","last":1,"pkts":330,"rssi":-61}, ...]}
   ```

   It also prints one JSON line per validated detection (the same payload goes to
   MQTT):

   ```
   {"mac":"00:25:DF:..","alias":"","rssi":-66,"type":"OUI","match":"0025DF",
    "desc":"AXON [mesh D4E5F6]","node":"D4E5F6","dom":"ble"}
   ```

   `node` is the stable ID (eFuse MAC tail) of the node whose radio saw the device.
   `dom` is `ble` or `wifi`.

2. **MQTT.** The home node's detection publishes go to `<device_id>/detection`,
   and `<device_id>/availability` reports whether it is online. MQTT carries
   detections only, not the roster, so live link telemetry needs serial.

Older firmware without `TLM` still works. The dashboard falls back to the
`[HEARTBEAT]` counters and the `[MESH] node X joined/offline` lines, and reads the
reporting node from the `[mesh XXXXXX]` suffix in `desc`. Per-node RSSI and packet
counts need the TLM firmware.

Opening the port never resets the board. The S3's USB-JTAG does wire DTR/RTS to
reset, but the dashboard uses a plain open and close, which change both lines
in one atomic step. It never toggles them separately, which would pulse a
reset. The port disappears on every reboot, and the dashboard re-detects it
within half a second and reconnects. If an open port goes silent for 12 s
(firmware prints every 5 s), the dashboard reopens it.

The header's USB indicator shows the data, not just the port:

- **LIVE** (green): lines are arriving. It blinks on every line.
- **PORT OPEN, NO DATA** (amber): the port is open but nothing has arrived for 7 s.
- **DISCONNECTED** (red): the board is unplugged. This shows the moment the port goes away.

## Features

- **Fleet panel.** Every node in the roster, keyed by stable node ID, with role,
  online/stale/offline state, last seen, packet count, link RSSI and detection
  count. The home node's counters (RX/TX/TX-fail/dup, lRSSI, uptime, MQTT,
  channel-heal) sit above the list.
- **Map.** Drag markers to reposition nodes. Use "Place on map" (or the "Not on
  map" chips) and click to drop a node. Positions are saved locally. Markers are
  coloured by status, labelled with alias and RSSI, and burst red when that node
  reports a detection. A red badge counts each node's detections in the last 15
  minutes. Dashed lines join the home node to each remote, coloured by the link
  RSSI the home hears. Select a node to label its links with dBm and distance,
  which is handy for range testing.
- **Aliases and notes.** Friendly names for nodes (keyed by node ID) and for
  detected devices (keyed by MAC), used everywhere in the UI and included in
  exports. History search matches device aliases too.
- **Devices tab.** One row per MAC: vendor, type, hit count, first/last seen,
  best RSSI, and which nodes saw it. Click a row for the device drawer, which
  has alias/notes, per-node hit counts and every sighting.
- **Live detections.** Pushed over Socket.IO. A MAC the dashboard has never seen
  before is tagged `NEW`. A 24 h detections-per-hour chart sits above the feed.
  Optional sound (WebAudio beeps) and screen-edge flash.
- **Health toasts.** New devices, remotes joining or dropping off the mesh, the
  home node going silent or coming back, and channel-heal sweeps. Opt-in desktop
  notifications (from your browser, shown while the tab is in the background)
  are in Settings.
- **History.** SQLite (`ouispy.db`). Search by MAC, alias, vendor or type, and
  filter by node, BLE/WiFi and time range. Export detections as CSV/JSON
  (honouring the filters), and export node sightings (online/offline events plus
  RSSI samples once a minute).
- **Node drawer.** Status, 24 h link-RSSI sparkline, recent detections, and
  online/offline events.
- **Console.** The raw serial stream, colour-coded, plus a box to send console
  commands (e.g. `apcheck`, `meshrechan 6`) to the home node over USB.
- **Remote settings (node drawer).**
  - **Readout:** each node's firmware build, scan/config mode, burned-in, fleet key, uptime, heap, delivery stats and filter count. The filter count carries a SAME AS HOME / DIFFERS badge, from the CRC of the node's list compared with the home's.
  - **Controls:** buzzer/LED switches, identify (beep + flash for 15 s, ignoring mute), install a preset, and reboot.
  - **Filter editor:** read the node's filter list (it always starts from what's installed), edit it, copy the home's list, or add from the picker, then push it. Validation runs live: OUIs need 6 hex digits, company IDs and service UUIDs 4, full MACs 12, names 1-16 characters.
  - **Predefined filters:** every firmware preset and every vendor in the portal's OUI database (read live from `BLEOUIs.md` / `WiFiOUIs.md`), each marked INSTALLED / PARTIAL / not installed for that node, with Add and Remove. Changes are staged and go out with Push, so any node, burned in or not, can gain or drop detections live. BLE entries add each OUI as a BLE filter and/or a WiFi probe filter (the BLE/WiFi switches, like the portal's). WiFi entries add probe and/or beacon filters per their frame types.
  - **Commands:** every command shows its state (queued, waiting, done, failed) and each node's reply. Destructive actions take two taps.
- **Fleet Control tab.** Fleet-wide versions of the same actions: read all settings/filters, identify all, buzzer/LED for all remotes, push the home's filters, install a preset on every remote, reboot all, and re-channel the whole fleet. A settings matrix lists every node, and the command log can include the automatic reads.
- **Settings.** Serial port, MQTT broker/credentials/topic, map tiles,
  notifications, link lines, and history retention (days to keep, 0 = forever,
  pruned hourly). Feed changes restart the feeds live.

## How remote settings work

The dashboard types console commands to the home node over USB (`rstatus`,
`raudio`, `rfilters`, `mfbegin`/`mfdata`/`mfend`, ... see the firmware README).
The home floods them over the mesh, addressed to one node or to all. Replies come
back as single lines that the dashboard matches to the command:

- `CMD {"id":..}`: the home accepted the command and sent it
- `ACK {"node":..,"id":..,"st":..}`: a node applied it (`st` 0) or refused it
  (1 rejected or old firmware, 2 fleet key mismatch, 3 replayed)
- `RST {...}`: a node's settings
- `RFL {...}`: a node's filter list

One command is on the wire at a time. "Mesh busy" refusals (an earlier filter
transfer still going out) are retried. A node's settings are read when it
joins, after every change, and every `status_poll` seconds (Settings; 0 turns
it off). This needs the USB feed: MQTT carries detections only. Every node must
run firmware with remote management. Older nodes show "rejected" or "no reply".
With a fleet key set, the home signs every command, so set the same key on all
nodes (`meshkey <secret>` on each node's console).

Demo mode simulates all of this. One demo node (Lot B) runs "old firmware" and
refuses the new commands, so the failure paths are visible too.

## Tests

```bash
python -m unittest discover -s tests -v
```

Covers the line parser (TLM, detection JSON, legacy firmware lines, garbage
input), roster offline detection, cross-source dedup, device aggregation, alias
search, hourly buckets and retention pruning. Also covers the filter wire format
and validation, command planning, and full commander round trips (status,
audio, filter push and readback, an old-firmware refusal, local home commands)
against the demo console simulator.

## Fleet key

With a fleet key set, the whole mesh is sealed (AES-256-GCM). Each node gets the
key over its USB console (`meshkey <key>`) or through its setup portal (Mesh
section, Fleet key). It never goes over the air. The dashboard reads the key from
`~/.config/ouispy/fleet_key` on the computer running it, outside the repo.

**Settings, Fleet key, Show key as QR** displays it as a QR code, which hides
itself after 60 s. Scan it on your phone, then paste it into a new node's portal.
The key is only shown on that computer and your Tailscale devices, never to the plain
LAN.

## Privacy notes

- The server listens on all interfaces. This computer, your LAN and Tailscale
  open it directly; any other network needs the access token in
  `home-dashboard/access_token` (`?token=...` once, then a cookie).
  `--require-token` demands it from everything except this computer, and
  `--host 127.0.0.1` keeps it local-only. Socket.IO only accepts same-origin
  connections.
- Leaflet and the Socket.IO client are vendored in `static/vendor/`, so the page
  loads no third-party scripts.
- Map tiles are the one thing that goes online. By default they come from
  OpenStreetMap, which tells the tile server roughly where you are looking. To
  keep that private, choose a local tile server or the offline grid in Settings.
