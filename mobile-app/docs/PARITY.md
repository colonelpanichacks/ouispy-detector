# Parity: everything the Tab5 and the dashboard do, in the app

The app is the dashboard (see the README): plugged into a node it does everything itself,
and on the same network it can use the home dashboard instead. This is the checklist of
every Tab5 view and dashboard feature, from a full read of `tab5-dashboard/src/ui.cpp` and
`home-dashboard/static/js/app.js`, and where each one stands in the app.

**done** = in the app and working · **part** = some of it · **todo** = not yet ·
**fw** = needs a firmware change first · **dash** = only through the home dashboard

Each item also notes how it works when the app is the dashboard (USB) and when it uses the
home dashboard.

## Sources and links

| Feature | Status | Notes |
|---|---|---|
| Home dashboard: REST + Socket.IO, token cookie, mDNS `_ouispy._tcp`, typed host / Tailscale | done | |
| Node on USB (Android OTG): the app is the dashboard | done | `local_mesh.dart`; DTR/RTS safe |
| Source picker Auto / USB / Dashboard; auto-switch on plug / unplug | done | |
| OUI-SPY Recon on USB (Blue mesh lines) | done | same TLM / RST / detection lines |
| Node over BLE (iOS, and Android without a cable) | fw | needs a GATT line service on the node (PHASE2.md) |
| Recon Blue modes' own lines (fox hunt, Flock, sniff, PCAP, Sky-Spy C5) | todo | parse and show per mode |
| Phone GPS: own position, trail, tag detections, place a node where you stand | todo | geolocator added |
| Bridge to the dashboard (Tab5 `bridge_lines`) | todo | phone on USB forwarding lines to the dashboard |

## Header, status, alerts

| Feature | Status | Notes |
|---|---|---|
| Link badge (LIVE / HOME DOWN / DEMO / REPLAY / NO LINK / USB) | done | under the title |
| Alert rule: live detection of a MAC unheard for 600 s, not muted | done | |
| Alert banner, ACK, SILENCE all, MUTE device | done | |
| Phone notification, vibration, sound switches | done | |
| Watch list: alert whenever chosen devices / vendors / names show up | todo | the Detections area |
| Drone appears toast + sound | part | toast on USB; sound todo |
| Node down / back toast + sound | part | toast on USB |
| Lost node (`lost_node`) alert | todo | |
| Proximity mode (pulse / beep faster as RSSI rises, STOP) | todo | Tab5 device detail |
| Sound page: per-event on/off, test buttons, volume | todo | |

## Overview / Feed

| Feature | Status | Notes |
|---|---|---|
| KPIs: nodes online, detections 24 h / all time, devices now, drones live, alerts, known MACs | done | |
| 24 h activity chart | done | |
| Live feed, filter ALL / BLE / WIFI / DRONE | done | |
| Feed filter by node; by kind (detector / sky-spy / wardriver) | todo | |
| Wardrive mode KPIs and feed | todo | |
| Live intel panel (trend closing in / moving off, hide on map) | todo | dashboard |
| Firmware-update strip (nodes mid-OTA) | todo | |

## Devices (Live / Targets / History)

| Feature | Status | Notes |
|---|---|---|
| Heard now, with seen-by nodes and tri badge | done | |
| History search (MAC, name, vendor, alias), domain filter | done | |
| Sort columns, node filter, group filter, kind filter (detected / wardriven) | todo | |
| Export CSV / JSON (detections, sightings, WiGLE) | todo | share sheet |
| Clear detections (all / node / device) | todo | |

## Device detail

| Feature | Status | Notes |
|---|---|---|
| Name, notes, group (pick / new / remove), mute | done | |
| Live signal, last heard, heard-by per node | done | |
| Over-the-air intel rows, vendor / company / appearance / services (offline lookup) | done | |
| Turns up with (companions) | done | phone computes them on USB |
| Triangulated position map | done | tri.py ported |
| Range circles per node, device trail on the map | todo | |
| Triangulation calibration at a spot | todo | |
| Proximity mode | todo | |
| Add as detection (install a filter for it on a node / all nodes) | todo | Detections area |
| Drone devices link to the drone page | todo | |

## Map

| Feature | Status | Notes |
|---|---|---|
| Dashboard tiles, layer picker, on-phone tile cache, save an area offline | done | |
| Nodes, mesh links, triangulated targets with error circles, drones, pilots, tracks | done | |
| Marks: show pins and areas | done | |
| Drop a pin (long-press) | part | name only |
| Pin editor: name, colour (36 hues), icon (12), save / delete | todo | |
| Draw an area: tap corners, undo, done (3-24), name, colour, edit, delete | todo | |
| Marks kept on the phone and synced to the dashboard | part | USB keeps them; sync todo |
| Toggles TRI / TRACKS / PILOT / WD (off / now / all) / ME (follow GPS) | part | targets, drones, links, nodes, marks |
| Node card: settings, MOVE (tap to place), track on/off, lock | todo | |
| Drone card: lock on drone / pilot, track on/off, flights, details | todo | |
| Past flight on the map with takeoff / landing marks | part | on the drone page |
| Wardriven devices layer | todo | |
| Own GPS dot, heading and trail | todo | |
| Double-tap zoom, pinch | done | |
| Coverage overlay (expected error) | todo | dashboard |

## Drones

| Feature | Status | Notes |
|---|---|---|
| Window 1H / 4H / 8H / 24H / ALL / SPAN | done | |
| Live list, window list, detail with every Remote ID field | done | |
| Tags, rename, notes | done | |
| Past flights, drawn on the map | done | |
| Instant Remote ID lookup: maker from the serial's manufacturer code (offline) | todo | |
| FAA registry make / model lookup | todo | on the phone and through the dashboard |
| Show on map, follow live drone, radar box round the pilot | todo | |
| KPIs (live, airborne, pilots located, highest) | todo | |

## FPV (the Recon's 5.8 GHz receiver)

| Feature | Status | Notes |
|---|---|---|
| FPV tab: detections, channel / band scan, signal, video feed, every control the Recon offers | todo | researching what the receiver exposes |

## Detections (what the nodes and the phone alert on)

| Feature | Status | Notes |
|---|---|---|
| Watch list on the phone (MAC, OUI, vendor, name, company ID, service), alert on every sighting | todo | |
| Presets (Axon, Meta, i-PRO, Verkada) and the vendor library | part | preset install on a node exists |
| Read a node's filters, live add / remove, full editor, push (mfbegin / mfdata / mfend) | todo | CRC matches `fcrc` |
| Install on every node; copy the home's list; meshpush | todo | |
| Recon: presets through its portal; MAC / OUI lists over USB / BLE | fw | Recon refuses mesh filter commands |

## Fleet

| Feature | Status | Notes |
|---|---|---|
| Node list, home link counters, node page with status and neighbours | done | |
| Buzzer, LED, identify, beep, locate, read status, preset, rename, colour, reboot | done | |
| Fleet-wide: read all, identify all, audio all, reboot remotes | todo | |
| Re-channel the fleet, push home filters (meshpush) | todo | |
| Wardriver controls: channels plan, raw packets, new session | todo | |
| Firmware: install a kind (detector / sky-spy / wardrive / gps) on a node, fleet update | dash | phone-side flashing is phase 2 |
| Lost / found, deny, wipe a node / all | todo | |
| Forget an offline node, remove from map | todo | |
| Fleet key status, create, show as QR, set on home | todo | USB only, never shown in logs |
| Who-hears-whom matrix, mesh status tiles | todo | |
| Command log (jobs, replies) | todo | |

## Metrics

| Feature | Status | Notes |
|---|---|---|
| Range 1H / 6H / 24H / 7D, node, domain | todo | |
| Detections: KPIs, over time, by vendor, by node, RSSI histogram, heatmap | todo | |
| Wardrive and drone metrics | todo | |

## Console

| Feature | Status | Notes |
|---|---|---|
| Live console lines, colour-coded, send a line | todo | USB: the node's console itself |

## Settings

| Feature | Status | Notes |
|---|---|---|
| Dashboard host, port, token, find on LAN | done | |
| Alerts on the phone, muted list, map cache, app update check | done | |
| Triangulation model (p0, n, sigma, smoothing) | todo | |
| Data retention, clear history, export / import | todo | |
| Connectors (MQTT, TAK, Reticulum, Meshtastic) | dash | dashboard settings, shown read-only |
