#!/usr/bin/env python3
"""OUI-SPY Mesh: live mesh telemetry, map, history and remote control for the detector fleet (via its HOME node).

    pip install -r requirements.txt
    python ouispymesh.py                     # serial auto-detect (+ MQTT if configured)
    python ouispymesh.py --demo              # simulated fleet, no hardware
    python ouispymesh.py --mqtt 192.168.1.10 --topic ouispy/detection
    python ouispymesh.py --replay capture.log

Local only: binds 127.0.0.1 by default, data lives in SQLite next to this file.
"""
import argparse
import hmac
import ipaddress
import os
import re
import secrets
import socket
import threading
import time

from flask import Flask, Response, abort, jsonify, redirect, render_template, request, url_for
from flask_socketio import SocketIO

import filters as flt
import firmware as fw
from commands import Commander, PlanError
from commands import plan as plan_cmd
from fleet import Fleet
from sources import (DemoSource, MqttSource, ReplaySource, SerialSource, Source,
                     WardriveUsbSource, list_serial_ports)
from store import Store

HERE = os.path.dirname(os.path.abspath(__file__))

DEFAULT_SETTINGS = {
    "sound": True,
    "flash": True,
    "tiles": "osm-dark",            # osm-dark | osm | custom-dark | custom | none
    "tiles_custom": "",
    "notify": False,                # browser notifications on detections / node drops
    "retention_days": 0,            # 0 = keep history forever
    "links": True,                  # draw home-to-remote link lines on the map
    "status_poll": 120,             # s between automatic settings reads (0 = off)
    # Access, as CIDR lists set per installation (Settings): who opens the
    # dashboard without the token, and who besides this computer may view
    # the fleet key. Add your own VPN's range to either if you use one.
    "trusted_nets": "192.168.0.0/16, 10.0.0.0/8, 172.16.0.0/12",
    "key_nets": "",
    "map_view": None,               # [lat, lon, zoom]
    # Triangulation signal model: RSSI at 1 m per radio (dBm), path-loss
    # exponent (2 open air .. 4 cluttered), per-node offsets in dB from
    # calibration (a device held next to the node), smoothing 0..1.
    # sigma = RSSI noise (dB) the error bars assume; max_err = largest 1-sigma
    # error (m) still shown as a fix; devices = per-device calibration
    # (MAC -> 1 m power, exponent, fit RMS, nodes used, when).
    "tri": {"p0_ble": -59, "p0_wifi": -40, "n": 2.7, "sigma": 4.0, "max_err": 8.0,
            "offsets": {}, "devices": {}, "smooth": 0.35},
    "serial_port": "auto",          # auto | off | a port: COM5, /dev/ttyACM0, /dev/cu.usbmodemXXXX
    "mqtt": {"enabled": False, "host": "", "port": 1883, "user": "", "password": "",
             "topic": "+/detection"},
}


def parse_args():
    ap = argparse.ArgumentParser(description="OUI-SPY home node dashboard")
    ap.add_argument("--host", default=os.environ.get("OUISPY_HOST", "0.0.0.0"),
                    help="bind address (default all interfaces; 127.0.0.1 = this computer only)")
    ap.add_argument("--open", action="store_true",
                    help="no access token from anywhere (not recommended: the dashboard can reboot the fleet)")
    ap.add_argument("--require-token", action="store_true",
                    help="require the token from every device except this computer, trusted networks included")
    ap.add_argument("--port", type=int, default=int(os.environ.get("OUISPY_PORT", 5077)))
    ap.add_argument("--no-advertise", action="store_true",
                    help="don't announce the dashboard on the LAN (mDNS _ouispy._tcp, used by the Tab5 display)")
    ap.add_argument("--serial", help="serial port, 'auto' (USB VID 0x303A) or 'off'")
    ap.add_argument("--mqtt", help="MQTT broker host[:port] (enables MQTT)")
    ap.add_argument("--mqtt-user")
    ap.add_argument("--mqtt-pass")
    ap.add_argument("--topic", help="MQTT detection topic filter (default +/detection)")
    ap.add_argument("--demo", action="store_true", help="simulated fleet, separate database")
    ap.add_argument("--demo-center", metavar="LAT,LON", help="where the demo fleet sits (default Portland, OR)")
    ap.add_argument("--demo-showcase", action="store_true", help="screenshot-safe demo data: no police/ALPR vendors, synthetic MACs")
    ap.add_argument("--replay", help="replay a recorded serial log")
    ap.add_argument("--speed", type=float, default=1.0, help="replay speed factor")
    ap.add_argument("--record", help="append raw serial lines (with timestamps) to this file")
    ap.add_argument("--db", help="SQLite path (default ouispy.db / ouispy-demo.db)")
    return ap.parse_args()


args = parse_args()
offline_mode = bool(args.demo or args.replay)
db_path = args.db or os.path.join(HERE, "ouispy-demo.db" if offline_mode else "ouispy.db")

app = Flask(__name__)
# Always render the page from the template on disk: the page and its versioned
# script must change together, even if the files are edited while running.
app.config["TEMPLATES_AUTO_RELOAD"] = True

# ----------------------------------------------------------------------
# Access control. The dashboard can reboot, re-channel and re-filter the
# fleet, so anything that isn't this computer needs a token: open the printed
# link (?token=...) once and the browser keeps it in a cookie.
# ----------------------------------------------------------------------
TOKEN_FILE = os.path.join(HERE, "access_token")


def _load_token():
    try:
        with open(TOKEN_FILE) as f:
            t = f.read().strip()
            if t:
                return t
    except OSError:
        pass
    t = secrets.token_urlsafe(18)
    with open(TOKEN_FILE, "w") as f:
        f.write(t + "\n")
    os.chmod(TOKEN_FILE, 0o600)
    return t


ACCESS_TOKEN = _load_token()
COOKIE = "ouispy_token"


LOOPBACK = ("127.0.0.1", "::1", "::ffff:127.0.0.1")


def parse_nets(text):
    """'192.168.0.0/16, 10.0.0.0/8' -> ([networks], [entries that aren't CIDRs])."""
    nets, bad = [], []
    for part in str(text or "").replace(";", ",").replace("\n", ",").split(","):
        part = part.strip()
        if not part:
            continue
        try:
            nets.append(ipaddress.ip_network(part, strict=False))
        except ValueError:
            bad.append(part)
    return nets, bad


ACCESS = {"trusted": [], "key": []}   # parsed from settings; refreshed when they change


def refresh_access():
    s = settings()
    ACCESS["trusted"] = parse_nets(s.get("trusted_nets"))[0]
    ACCESS["key"] = parse_nets(s.get("key_nets"))[0]


def _client_ip():
    """Who is really asking. Behind a reverse proxy on this computer the
    direct peer is loopback and the real client is in X-Forwarded-For, which
    is only trusted when the peer IS loopback. A proxy that exposes the
    dashboard publicly therefore forwards public addresses: never trusted."""
    peer = request.remote_addr or ""
    if peer in LOOPBACK:
        fwd = request.headers.get("X-Forwarded-For", "").split(",")[0].strip()
        if fwd:
            return fwd
    return peer


def _is_local():
    return _client_ip() in LOOPBACK


def _in(nets):
    try:
        ip = ipaddress.ip_address(_client_ip())
    except ValueError:
        return False
    if getattr(ip, "ipv4_mapped", None):
        ip = ip.ipv4_mapped
    return any(ip in n for n in nets)


def _is_trusted_net():
    return _in(ACCESS["trusted"])


# The fleet key (kept on this computer, outside the repo) can be shown as a
# QR for provisioning a node from a phone: only to this computer, plus the
# networks listed in the key_nets setting (empty by default).
FLEET_KEY_FILE = os.path.expanduser("~/.config/ouispy/fleet_key")


def _key_viewer_ok():
    return _is_local() or _in(ACCESS["key"])


def _fleet_key():
    try:
        with open(FLEET_KEY_FILE) as f:
            return f.read().strip() or None
    except OSError:
        return None


def key_fingerprint(key):
    """Same as the firmware's: SHA-256("OUISPY-KEY-FP" | key)[0:4], hex.
    One-way and short: tells whether nodes hold the same key, can't recover it."""
    import hashlib
    return hashlib.sha256(b"OUISPY-KEY-FP" + key.encode()).hexdigest()[:8].upper() if key else None


def _authorized():
    if args.open or _is_local():
        return True
    if not args.require_token and _is_trusted_net():
        return True
    got = request.cookies.get(COOKIE) or request.args.get("token") or ""
    return hmac.compare_digest(got, ACCESS_TOKEN)


@app.before_request
def _gate():
    if request.path.startswith("/socket.io"):
        return None   # checked in the connect handler
    if not _authorized():
        return Response("OUI-SPY Mesh: open the link with ?token=... printed where it runs "
                        "(or in home-dashboard/access_token).", 401, mimetype="text/plain")
    if request.args.get("token") and request.path == "/":
        # Trade the URL token for a long-lived cookie and drop it from the URL.
        resp = redirect("/")
        resp.set_cookie(COOKIE, ACCESS_TOKEN, max_age=365 * 86400, httponly=True, samesite="Strict")
        return resp
    return None
socketio = SocketIO(app, async_mode="threading")  # same-origin only
store = Store(db_path)


def emit(event, payload):
    socketio.emit(event, payload)


fleet = Fleet(store, emit)
import drones as drones_mod
drones = drones_mod.Drones(store, emit)
fleet.drones = drones
drones.on_det = fleet.feed_detection
import wardrive as wardrive_mod
wardrive = wardrive_mod.Wardrive(store, emit)
fleet.wardrive = wardrive
import tri as tri_mod


def _node_pos(nid):
    m = store.nodes_meta().get(nid) or {}
    return (m["lat"], m["lon"]) if m.get("lat") is not None and m.get("lon") is not None else None


fleet.tri = tri_mod.Tri(lambda: store.all_settings(), _node_pos)
sources = {}
# Re-entrant: a source reports its status while being registered (set_state ->
# on_source_status -> sources_status), and a plain Lock held by that same thread
# deadlocked the whole app (every /api/state then queued behind it).
src_lock = threading.RLock()


def command_sender():
    """The feed that can carry console commands: live serial, a node bridged by a Tab5
    display, or the demo."""
    with src_lock:
        for key in ("serial", "bridge"):
            ser = sources.get(key)
            if ser and ser.status().get("connected"):
                return ser
        return sources.get("demo")


def _settings_for_commander():
    return settings()


commander = Commander(fleet, store, emit, command_sender, _settings_for_commander)
fleet.on_line = commander.feed_line
fleet.on_join = commander.node_joined


def settings():
    s = dict(DEFAULT_SETTINGS)
    s.update(store.all_settings())
    s["mqtt"] = {**DEFAULT_SETTINGS["mqtt"], **(s.get("mqtt") or {})}
    return s


def public_settings():
    s = settings()
    s["mqtt"] = {**s["mqtt"], "password": "********" if s["mqtt"].get("password") else ""}
    return s


def sources_status():
    with src_lock:
        return {k: v.status() for k, v in sources.items()}


def on_source_status():
    emit("sources", sources_status())


def start_sources():
    """(Re)start every feed from CLI flags + saved settings."""
    with src_lock:
        for s in sources.values():
            s.stop()
        sources.clear()
    s = settings()
    new = {}
    if args.demo:
        if args.demo_showcase:
            import sources as _src
            _src.use_showcase()
        new["demo"] = DemoSource(fleet, on_source_status)
        _seed_demo_positions()
    elif args.replay:
        new["replay"] = ReplaySource(fleet, on_source_status, args.replay, args.speed)
    else:
        port = args.serial or s["serial_port"]
        if port != "off":
            new["serial"] = SerialSource(fleet, on_source_status, port, record=args.record,
                                         get_pref=lambda: store.get_setting("home_usb_serial"),
                                         set_pref=lambda sn: store.set_setting("home_usb_serial", sn),
                                         get_ignore=lambda: store.get_setting("usb_ignore", []))
            # Wardrivers on this computer's USB, besides the home.
            new["wardrive-usb"] = WardriveUsbSource(
                fleet, on_source_status,
                busy=lambda: [s.cur_sn for s in sources.values() if isinstance(s, SerialSource) and s.ser and s.cur_sn]
                + [store.get_setting("home_usb_serial")],
                get_ignore=lambda: store.get_setting("usb_ignore", []), wardrive=wardrive)
        m = s["mqtt"]
        if args.mqtt:
            host, _, p = args.mqtt.partition(":")
            m = {**m, "enabled": True, "host": host, "port": int(p or m["port"] or 1883)}
            if args.mqtt_user:
                m["user"] = args.mqtt_user
            if args.mqtt_pass:
                m["password"] = args.mqtt_pass
            if args.topic:
                m["topic"] = args.topic
        if m.get("enabled") and m.get("host"):
            new["mqtt"] = MqttSource(fleet, on_source_status, m["host"], m["port"],
                                     m.get("user", ""), m.get("password", ""), m.get("topic"))
    with src_lock:
        sources.update(new)
    for src in new.values():
        src.start()
    on_source_status()


def _seed_demo_positions():
    """Give the fake fleet a layout the first time demo mode runs."""
    from sources import DEMO_HOME, DEMO_REMOTES
    meta = store.nodes_meta()
    base = tuple(float(v) for v in args.demo_center.split(",")) if args.demo_center else (45.5231, -122.6765)
    offsets = {DEMO_HOME: (0, 0), DEMO_REMOTES[0]: (0.0042, -0.0061),
               DEMO_REMOTES[1]: (-0.0035, -0.0072), DEMO_REMOTES[2]: (0.0068, 0.0081),
               DEMO_REMOTES[3]: (-0.0049, 0.0057)}
    names = {DEMO_HOME: "Base", DEMO_REMOTES[0]: "North Gate",
             DEMO_REMOTES[1]: "Overpass", DEMO_REMOTES[2]: "Ridge Relay",
             DEMO_REMOTES[3]: "Lot B"}
    for nid, (dlat, dlon) in offsets.items():
        m = meta.get(nid) or {}
        if m.get("lat") is None:
            store.update_node(nid, lat=base[0] + dlat, lon=base[1] + dlon)
        if not m.get("alias"):
            store.update_node(nid, alias=names[nid])
    _backfill_demo_history()


def _backfill_demo_history():
    """24 h of fake sightings + detections so charts and history have shape."""
    import random
    from sources import DEMO_HOME, DEMO_REMOTES, DEMO_TARGETS, demo_mac
    if store.query_detections(limit=1)[0]:
        return
    now = time.time()
    base_rssi = {"DE0B02": -58, "DE0C03": -71, "DE0D04": -84, "DE0E05": -66}
    for nid in DEMO_REMOTES:
        r, pkts = base_rssi[nid], 0
        for i in range(144):
            ts = now - 86400 + i * 600
            r = max(-95, min(-45, r + random.randint(-3, 3) + (base_rssi[nid] - r) // 4))
            pkts += random.randint(2500, 3500)
            store.add_sighting(nid, "sample", r, pkts, ts)
        if nid == "DE0D04":
            for h in (20, 13, 5):
                store.add_sighting(nid, "offline", None, None, now - h * 3600)
                store.add_sighting(nid, "online", None, None, now - h * 3600 + 900)
    nodes = [DEMO_HOME] + DEMO_REMOTES
    for i in range(70):
        oui, ftype, label, dom = random.choice(DEMO_TARGETS)
        mac = demo_mac(oui)
        store.add_detection({"ts": now - random.uniform(600, 86400), "node": random.choice(nodes),
                             "mac": mac, "alias": "", "rssi": random.randint(-92, -48),
                             "type": ftype, "match": oui.replace(":", ""), "descr": label,
                             "dom": dom, "source": "demo", "is_new": random.random() < 0.3})


def ticker():
    last_prune = 0
    while True:
        time.sleep(1)
        fleet.tick()
        if time.time() - last_prune > 3600:
            last_prune = time.time()
            removed = store.prune(int(settings().get("retention_days") or 0))
            if removed:
                print(f"retention: pruned {removed} old rows")


# ----------------------------------------------------------------------
# HTTP
# ----------------------------------------------------------------------
@app.after_request
def no_cache(resp):
    # The page itself is never cached (phones held on to an old one after
    # updates); its scripts and styles are versioned by asset() instead.
    if request.path.startswith("/api/") or request.path == "/":
        resp.headers["Cache-Control"] = "no-store"
    return resp


@app.template_global()
def asset(filename):
    """Static URL with the file's mtime as ?v=, so an update is a new URL."""
    try:
        v = int(os.path.getmtime(os.path.join(app.static_folder, filename)))
    except OSError:
        v = 0
    return url_for("static", filename=filename, v=v)


@app.route("/")
def index():
    return render_template("index.html", demo=args.demo, replay=bool(args.replay))


def _filters():
    a = request.args
    return {"q": a.get("q") or None, "node": a.get("node") or None,
            "dom": a.get("dom") or None, "mac": a.get("mac") or None,
            "since": a.get("since") or None, "until": a.get("until") or None}


@app.route("/api/state")
def api_state():
    total, recent = store.query_detections(limit=150)
    store.annotate_seen_by(recent)
    return jsonify({"fleet": fleet.snapshot(), "sources": sources_status(),
                    "settings": public_settings(), "stats": store.detection_stats(),
                    "hourly": store.hourly(), "mac_aliases": store.mac_aliases(),
                    "device_names": store.device_names(), "groups": store.groups(),
                    "drones": drones.snapshot(), "drone_tags": drones_mod.TAGS,
                    "tri": list(fleet.tri_fixes.values()),
                    "group_of": store.group_of(),
                    "jobs": _jobs(None), "can_command": command_sender() is not None,
                    "filter_meta": _filter_meta(),
                    "recent": recent, "console": fleet.console_tail(),
                    "mode": "demo" if args.demo else ("replay" if args.replay else "live")})


@app.route("/api/detections")
def api_detections():
    limit = min(int(request.args.get("limit", 200)), 2000)
    offset = int(request.args.get("offset", 0))
    total, rows = store.query_detections(limit=limit, offset=offset, **_filters())
    return jsonify({"total": total, "rows": rows})


@app.route("/api/export/detections.<fmt>")
def api_export_det(fmt):
    if fmt not in ("csv", "json"):
        abort(404)
    body = store.export_detections(fmt, **_filters())
    stamp = time.strftime("%Y%m%d-%H%M%S")
    return Response(body, mimetype="text/csv" if fmt == "csv" else "application/json",
                    headers={"Content-Disposition":
                             f"attachment; filename=ouispy-detections-{stamp}.{fmt}"})


@app.route("/api/export/sightings.<fmt>")
def api_export_sig(fmt):
    if fmt not in ("csv", "json"):
        abort(404)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    return Response(store.export_sightings(fmt),
                    mimetype="text/csv" if fmt == "csv" else "application/json",
                    headers={"Content-Disposition":
                             f"attachment; filename=ouispy-sightings-{stamp}.{fmt}"})


@app.route("/api/detections/clear", methods=["POST"])
def api_clear():
    body = request.get_json(silent=True) or {}
    node, mac = body.get("node") or None, body.get("mac") or None
    n = store.clear_detections(node, mac)
    emit("dets_cleared", {"node": node, "mac": mac, "n": n})   # every open dashboard drops them too
    return jsonify({"ok": True, "n": n})


@app.route("/api/metrics")
def api_metrics():
    rng = max(300, min(90 * 86400, int(request.args.get("range") or 86400)))
    until = time.time()
    return jsonify(store.metrics(until - rng, until, request.args.get("node") or None,
                                 request.args.get("dom") or None))


@app.route("/api/stats")
def api_stats():
    return jsonify({**store.detection_stats(), "hourly": store.hourly()})


@app.route("/api/devices")
def api_devices():
    f = _filters()
    node = f["node"]
    if isinstance(node, str) and node.lower().startswith("kind:"):   # every node running one firmware
        kind = node[5:].lower()
        node = [n["id"] for n in fleet.snapshot()["nodes"] if fw.node_kind((n.get("cfg") or {}).get("fw")) == kind]
    total, rows = store.devices(q=f["q"], node=node, dom=f["dom"],
                                limit=min(int(request.args.get("limit", 300)), 2000))
    return jsonify({"total": total, "rows": rows})


@app.route("/api/devices/<mac>", methods=["GET"])
def api_device_detail(mac):
    """Everything known about one device: what it said over the air (merged
    intel, with names for the numbers), its history, who heard it, its group
    and the devices that keep turning up with it."""
    import lookups
    mac = mac.upper()
    rec = store.device_intel(mac) or {}
    intel = lookups.decorate(rec.get("intel") or {})
    agg, hist = store.device_history(mac, limit=int(request.args.get("limit", 60)))
    alias = store.mac_aliases().get(mac, {})
    comps = store.companions(mac)
    names = store.device_names()
    groups = store.group_of()
    for c in comps:
        c["name"] = names.get(c["mac"], "")
        c["alias"] = store.mac_aliases().get(c["mac"], {}).get("alias", "")
        c["group"] = groups.get(c["mac"])
        c["vendor"] = lookups.oui_vendor(c["mac"])
    from fleet import intel_name
    if request.args.get("flat"):
        info = {"intel": intel, "mac_kind": lookups.mac_kind(mac), "oui_vendor": lookups.oui_vendor(mac), "stats": agg}
        return jsonify({"mac": mac, "name": intel_name(rec.get("intel")) or "", "alias": alias.get("alias", ""),
                        "group": groups.get(mac), "rows": _flat_intel(info, lookups),
                        "companions": [{"mac": c["mac"], "label": c["alias"] or c["name"] or c["descr"] or c["vendor"],
                                        "together": c["together"], "occasions": c["occasions"],
                                        "group": (c["group"] or {}).get("name", "")} for c in comps[:8]],
                        "groups": [{"id": g["id"], "name": g["name"], "n": len(g["members"])} for g in store.groups()]})
    return jsonify({
        "mac": mac, "name": intel_name(rec.get("intel")) or "",
        "alias": alias.get("alias", ""), "notes": alias.get("notes", ""),
        "group": groups.get(mac), "oui_vendor": lookups.oui_vendor(mac),
        "mac_kind": lookups.mac_kind(mac),
        "intel": intel, "intel_first": rec.get("first"), "intel_last": rec.get("last"),
        "intel_reports": rec.get("reports", 0),
        "stats": agg, "history": hist, "companions": comps,
    })


@app.route("/api/firmware/library")
def api_fw_library():
    return jsonify(fw.library())


@app.route("/api/wardrive")
def api_wardrive():
    scope = "history" if request.args.get("scope") == "history" else "session"
    return jsonify({"scope": scope, **wardrive.summary(),
                    "devices": wardrive.devices(scope, limit=int(request.args.get("limit", 20000)),
                                                placed=request.args.get("placed") == "1",
                                                node=(request.args.get("node") or "").upper() or None)})


@app.route("/api/wardrive/device/<mac>")
def api_wardrive_device(mac):
    d = wardrive.detail(mac, "ble" if request.args.get("kind") == "ble" else "wifi")
    return (jsonify(d), 200) if d else (jsonify({"error": "unknown device"}), 404)


@app.route("/api/wardrive/session", methods=["POST"])
def api_wardrive_session():
    return jsonify({"session": wardrive.new_session(str((request.get_json(silent=True) or {}).get("note") or ""))})


@app.route("/api/wardrive/wigle.csv")
def api_wardrive_csv():
    scope = "history" if request.args.get("scope") == "history" else "session"
    name = time.strftime("WigleWifi_%Y%m%d_%H%M%S.csv")
    return Response(wardrive.wigle_csv(scope), mimetype="text/csv",
                    headers={"Content-Disposition": f'attachment; filename="{name}"'})


@app.route("/api/drones")
def api_drones():
    return jsonify({"live": drones.snapshot(track=int(request.args.get("track", 60))),
                    "recent": drones.recent(hours=float(request.args.get("hours", 24))),
                    "tags": drones_mod.TAGS})


@app.route("/api/drones/<mac>", methods=["GET"])
def api_drone(mac):
    d = drones.detail(mac, since=float(request.args["since"]) if request.args.get("since") else None)
    return (jsonify(d), 200) if d else (jsonify({"error": "unknown drone"}), 404)


@app.route("/api/drones/<mac>", methods=["POST"])
def api_drone_meta(mac):
    data = request.get_json(silent=True) or {}
    drones.set_meta(mac, alias=(str(data["alias"])[:40].strip() if "alias" in data else None),
                    tag=data.get("tag"), notes=(str(data["notes"])[:500] if "notes" in data else None))
    return jsonify({"ok": True, "drone": drones.detail(mac)})


@app.route("/api/drones/<mac>/faa", methods=["POST"])
def api_drone_faa(mac):
    """On-demand FAA UAS DOC lookup of the drone's serial number (online)."""
    d = drones.detail(mac)
    serial = (request.get_json(silent=True) or {}).get("serial") or (d or {}).get("id")
    if not serial:
        return jsonify({"ok": False, "error": "no serial number broadcast yet"}), 400
    try:
        res = drones_mod.faa_lookup(serial)
    except Exception as e:
        return jsonify({"ok": False, "error": f"FAA lookup failed: {e}"}), 502
    drones.set_meta(mac, faa=res)
    return jsonify({"ok": True, "faa": res})


@app.route("/tiles/<layer>/<int:z>/<int:x>/<int:y>")
def tile_proxy(layer, z, x, y):
    import tiles
    try:
        data, mime = tiles.tile(layer, z, x, y)
    except KeyError:
        return ("", 404)
    except Exception as e:
        return (f"tile fetch failed: {e}", 502)
    resp = app.response_class(data, mimetype=mime)
    resp.headers["Cache-Control"] = "public, max-age=604800"
    return resp


@app.route("/api/tiles")
def api_tiles():
    import tiles
    return jsonify({k: {"name": v["name"], "max": v["max"], "ext": v["ext"]} for k, v in tiles.LAYERS.items()})


def _flat_intel(info, lookups):
    """Device detail as [label, value] text rows, for small displays."""
    it = info.get("intel") or {}
    kind = info.get("mac_kind") or {}
    rows = []
    def add(k, v):
        if v not in (None, "", [], {}):
            rows.append([k, str(v)])
    by_last = lambda o: sorted(o or {}, key=lambda k: -(o[k].get("last") or 0))
    add("Broadcast name", f'"{it["name"]}"' if it.get("name") else "")
    add("Earlier names", ", ".join(f'"{n}"' for n in by_last(it.get("names")) if n != it.get("name")))
    add("MAC vendor", info.get("oui_vendor") or ("randomized address" if kind.get("local") else "not registered"))
    if "rnd" in it:
        add("Address", ("random (" + kind.get("ble_random_kind", "") + ")") if it["rnd"] else "public")
    if "conn" in it:
        add("Connectable", "yes" if it["conn"] else "no")
    if it.get("cid"):
        add("Company ID", f'0x{it["cid"]} {it.get("cid_name", "")}'.strip())
    add("Manufacturer data", it.get("mfr"))
    add("Services", ", ".join(f'{u["uuid"]} {u["name"]}'.strip() for u in it.get("uuids_named") or []))
    add("128-bit service", it.get("uuid128"))
    add("Service data", ", ".join(f'{u["uuid"]}={u["data"]}' for u in it.get("sdata_named") or []))
    if it.get("app") is not None:
        add("Appearance", it.get("app_name") or it["app"])
    if it.get("tx") is not None:
        add("TX power", f'{it["tx"]} dBm')
    add("Frames", it.get("frame"))
    ssids = by_last(it.get("ssids"))
    ap = it.get("frame") in ("beacon", "probe-resp")
    add("SSID" if ap else "Looks for", ", ".join(s or "(hidden)" for s in ssids))
    add("Joins", ", ".join(f'{b} {it["aps"][b].get("ssid", "")}'.strip() for b in by_last(it.get("aps"))))
    add("Security", it.get("sec"))
    add("Channel", it.get("ch"))
    if it.get("phy"):
        add("Wi-Fi", "802.11" + it["phy"] + (f' (to {it["rate"]} Mb/s legacy)' if it.get("rate") else ""))
    if it.get("wps"):
        add("WPS", "on")
    add("Country", it.get("cc"))
    add("Vendor IEs", ", ".join(f'{v["oui"]} {v["name"]}'.strip() for v in it.get("vie_named") or []))
    st = info.get("stats") or {}
    if st.get("n"):
        add("Detections", f'{st["n"]}  best {st.get("best")} dBm  avg {round(st.get("avg") or 0)} dBm')
    return rows


@app.route("/api/groups", methods=["GET"])
def api_groups():
    return jsonify({"groups": store.groups(), "group_of": store.group_of()})


@app.route("/api/groups", methods=["POST"])
def api_group_save():
    """{id?, name, notes?, color?, add?: [mac], remove?: [mac]} -> saves the
    group and its membership. {id, delete: true} removes it."""
    data = request.get_json(silent=True) or {}
    gid = data.get("id")
    if data.get("delete") and gid:
        store.delete_group(int(gid))
    else:
        name = str(data.get("name") or "").strip()[:40]
        if not name and not gid:
            return jsonify({"ok": False, "error": "name required"}), 400
        if name or data.get("notes") is not None or data.get("color") is not None:
            cur = next((g for g in store.groups() if g["id"] == gid), {}) if gid else {}
            gid = store.save_group(int(gid) if gid else None, name or cur.get("name", ""),
                                   str(data.get("notes") if data.get("notes") is not None else cur.get("notes") or "")[:500],
                                   data.get("color", cur.get("color")))
        for m in data.get("add") or []:
            store.set_group(str(m).upper(), int(gid))
        for m in data.get("remove") or []:
            store.set_group(str(m).upper(), None)
    payload = {"groups": store.groups(), "group_of": store.group_of()}
    emit("groups", payload)
    return jsonify({"ok": True, "id": gid, **payload})


@app.route("/api/devices/<mac>", methods=["POST"])
def api_device_alias(mac):
    data = request.get_json(silent=True) or {}
    mac = mac.upper()
    cur = store.mac_aliases().get(mac) or {}
    # A field left out keeps its value (the Tab5 sends just the alias).
    alias = str(data["alias"] if "alias" in data else cur.get("alias", "") or "")[:40].strip()
    notes = str(data["notes"] if "notes" in data else cur.get("notes", "") or "")[:500] or None
    store.set_mac_alias(mac, alias, notes)
    aliases = store.mac_aliases()
    emit("mac_aliases", aliases)
    return jsonify({"ok": True, "mac_aliases": aliases})


@app.route("/api/nodes/<nid>", methods=["GET"])
def api_node(nid):
    nid = nid.upper()
    snap = fleet.snapshot()
    node = next((n for n in snap["nodes"] if n["id"] == nid), None)
    if node is None:
        abort(404)
    hours = float(request.args.get("hours", 24))
    _, dets = store.query_detections(limit=60, node=nid)
    stats = store.detection_stats()["per_node"].get(nid, {"count": 0, "last": None})
    return jsonify({"node": node, "sightings": store.node_sightings(nid, time.time() - hours * 3600),
                    "detections": dets, "stats": stats})


@app.route("/api/nodes/<nid>", methods=["POST"])
def api_node_update(nid):
    data = request.get_json(silent=True) or {}
    fields = {}
    if "alias" in data:
        fields["alias"] = str(data["alias"] or "")[:40].strip()
    if "notes" in data:
        fields["notes"] = str(data["notes"] or "")[:500]
    for k in ("lat", "lon"):
        if k in data:
            v = data[k]
            fields[k] = None if v is None else float(v)
    if "color" in data:   # hue 0-359, or null for the default colors
        v = data["color"]
        fields["color"] = None if v is None or v == "" else int(v) % 360
    store.update_node(nid.upper(), **fields)
    emit("fleet", fleet.snapshot())
    pushed = None
    if "alias" in fields and command_sender() is not None:
        # Push the name to the node itself (nodes on older firmware just
        # answer "unknown command"; the dashboard name still applies here).
        try:
            pushed = {"ok": True, "job": commander.submit("alias", nid.upper(), {"alias": fields["alias"]})}
        except Exception as e:      # PlanError (bad characters), no USB, ...
            pushed = {"ok": False, "error": str(e)}
    return jsonify({"ok": True, "pushed": pushed})


# GPS from a phone carrying the node: moves the node live and keeps a trail.
GPS_STATE = {}          # node -> {"ts", "acc", "by"} of the latest fix
TRAIL_SECONDS = 1800    # trail shown on the map: last 30 min


@app.route("/api/nodes/<nid>/gps", methods=["POST"])
def api_node_gps(nid):
    nid = nid.upper()
    d = request.get_json(silent=True) or {}
    try:
        lat, lon = float(d["lat"]), float(d["lon"])
        acc = float(d.get("acc") or 0) or None
    except (KeyError, TypeError, ValueError):
        return jsonify({"ok": False, "error": "lat/lon required"}), 400
    if not (-90 <= lat <= 90 and -180 <= lon <= 180):
        return jsonify({"ok": False, "error": "bad coordinates"}), 400
    now = time.time()
    store.update_node(nid, lat=lat, lon=lon)
    store.add_track(nid, lat, lon, acc, now)
    GPS_STATE[nid] = {"ts": now, "acc": acc}
    emit("gps", {"node": nid, "lat": lat, "lon": lon, "acc": acc, "ts": now})
    return jsonify({"ok": True})


@app.route("/api/nodes/<nid>/gps", methods=["DELETE"])
def api_node_gps_stop(nid):
    nid = nid.upper()
    GPS_STATE.pop(nid, None)
    if request.args.get("clear") == "1":
        store.clear_track(nid)
    emit("gps_stop", {"node": nid, "cleared": request.args.get("clear") == "1"})
    return jsonify({"ok": True})


@app.route("/api/tracks")
def api_tracks():
    secs = max(60, min(int(request.args.get("seconds", TRAIL_SECONDS)), 7 * 86400))
    return jsonify({"tracks": store.tracks(time.time() - secs), "seconds": secs,
                    "live": {k: v for k, v in GPS_STATE.items() if time.time() - v["ts"] < 60}})


@app.route("/api/nodes/<nid>", methods=["DELETE"])
def api_node_forget(nid):
    nid = nid.upper()
    with fleet.lock:
        st = fleet.nodes.get(nid)
        if st and st["online"]:
            return jsonify({"ok": False, "error": "node is online"}), 409
        fleet.nodes.pop(nid, None)
    store.forget_node(nid)
    emit("fleet", fleet.snapshot())
    return jsonify({"ok": True})


def _jobs(node, include_auto=False):
    live = {j["id"]: j for j in commander.recent(node, limit=100)}
    merged = dict(live)
    for j in store.recent_commands(node, limit=60, include_auto=include_auto):
        merged.setdefault(j["id"], j)
    rows = sorted(merged.values(), key=lambda j: j["created"], reverse=True)
    if not include_auto:
        rows = [j for j in rows if not j.get("auto")]
    return rows[:60]


def _filter_meta():
    vendors = [{"key": g["key"], "label": g["label"], "kind": g["kind"],
                "count": len(g.get("rows") or g["ouis"]), "ouis": g["ouis"],
                "rows": g.get("rows"), "meta": g["meta"]} for g in flt.vendor_db()]
    return {"types": flt.LABELS, "hex": flt.HEX_LEN, "library": flt.library(),
            "presets": {k: [{"t": t, "id": i, "d": d} for t, i, d in v] for k, v in flt.PRESETS.items()},
            "vendors": vendors}


@app.route("/api/filters/vendor")
def api_filters_vendor():
    """Filter rows for one OUI-database entry (?key=ble:3&ble=1&wifi=1)."""
    key = request.args.get("key", "")
    g = next((g for g in flt.vendor_db() if g["key"] == key), None)
    if not g:
        abort(404)
    return jsonify(flt.vendor_filters(g, request.args.get("ble", "1") == "1",
                                      request.args.get("wifi", "1") == "1"))


def _submit(op, target, args):
    try:
        return jsonify({"ok": True, "job": commander.submit(op, target, args)})
    except PlanError as e:
        return jsonify({"ok": False, "error": str(e)}), 400
    except (TypeError, ValueError) as e:
        return jsonify({"ok": False, "error": f"bad arguments: {e}"}), 400


@app.route("/api/nodes/<nid>/cmd", methods=["POST"])
def api_node_cmd(nid):
    data = request.get_json(silent=True) or {}
    op, args = str(data.get("op", "")), data.get("args") or {}
    if op == "wardrive":
        # A wardriver on this computer's USB: straight to its console (instant, and needs
        # nothing from the home's firmware); otherwise over the mesh through the home.
        with src_lock:
            wsrc = sources.get("wardrive-usb")
            bsrc = sources.get("bridge")
        try:
            lines, _, label = plan_cmd(op, nid.upper(), args, nid.upper())   # local form: "wd ..."
        except PlanError as e:
            return jsonify({"ok": False, "error": str(e)}), 400
        via = None
        if wsrc and all(wsrc.send_to(nid.upper(), ln) for ln, _ in lines):
            via = "USB"
        elif bsrc and bsrc.node_id == nid.upper():   # on the Tab5's USB
            for ln, _ in lines:
                bsrc.send(ln)
            via = "Tab5"
        if via:
            return jsonify({"ok": True, "job": {"id": 0, "label": f"{label} (via {via})", "state": "done", "op": op,
                                                "target": nid.upper(), "created": time.time(),
                                                "finished": time.time(), "error": "", "replies": {}}})
    return _submit(op, nid.upper(), args)


@app.route("/api/fleet/cmd", methods=["POST"])
def api_fleet_cmd():
    data = request.get_json(silent=True) or {}
    return _submit(str(data.get("op", "")), "all", data.get("args") or {})


@app.route("/api/fleetkey/status")
def api_fleetkey_status():
    k = _fleet_key()
    return jsonify({"present": k is not None, "viewable": _key_viewer_ok(), "fp": key_fingerprint(k)})


def _no_store(resp):
    resp.headers["Cache-Control"] = "no-store, max-age=0"
    resp.headers["Pragma"] = "no-cache"
    resp.headers["X-Content-Type-Options"] = "nosniff"
    return resp


@app.route("/api/fleetkey/reveal", methods=["POST"])
def api_fleetkey_reveal():
    """The key itself: POST only (never prefetched or cached), this computer
    and the key_nets networks only."""
    if not _key_viewer_ok():
        return _no_store(jsonify({"ok": False, "error": "the fleet key is only shown on the computer running OUI-SPY Mesh (and networks allowed in Settings)"})), 403
    k = _fleet_key()
    if not k:
        return _no_store(jsonify({"ok": False, "error": "no fleet key saved on this computer yet"})), 404
    return _no_store(jsonify({"ok": True, "key": k, "fp": key_fingerprint(k)}))


@app.route("/api/fleetkey/create", methods=["POST"])
def api_fleetkey_create():
    """Generate the fleet key if none exists. Never replaces one: a new key
    would cut off every node still holding the old one."""
    if not _key_viewer_ok():
        return jsonify({"ok": False, "error": "only from this computer (or networks allowed in Settings)"}), 403
    if _fleet_key():
        return jsonify({"ok": False, "error": "a fleet key already exists"}), 409
    import string
    alphabet = string.ascii_letters + string.digits
    k = "".join(secrets.choice(alphabet) for _ in range(32))
    os.makedirs(os.path.dirname(FLEET_KEY_FILE), exist_ok=True)
    try:
        os.chmod(os.path.dirname(FLEET_KEY_FILE), 0o700)
    except OSError:
        pass
    fd = os.open(FLEET_KEY_FILE, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(k + "\n")
    emit("fleetkey", {"fp": key_fingerprint(k)})   # status only, never the key
    return jsonify({"ok": True, "fp": key_fingerprint(k)})


@app.route("/api/fleetkey/provision_home", methods=["POST"])
def api_fleetkey_provision_home():
    """Set the fleet key on the HOME over USB. The key goes from the file on
    this computer straight down the serial line: never through the browser,
    never over the radio (remotes get it by USB or their setup portal)."""
    if not _key_viewer_ok():
        return jsonify({"ok": False, "error": "only from this computer (or networks allowed in Settings)"}), 403
    k = _fleet_key()
    if not k:
        return jsonify({"ok": False, "error": "no fleet key saved on this computer yet"}), 404
    home = fleet.home_id
    if not home:
        return jsonify({"ok": False, "error": "no home node on USB"}), 409
    try:
        return jsonify({"ok": True, "job": commander.submit("setkey", home, {"key": k})})
    except PlanError as e:
        return jsonify({"ok": False, "error": str(e)}), 400


@app.route("/api/jobs")
def api_jobs():
    return jsonify(_jobs(request.args.get("node") or None,
                         include_auto=request.args.get("auto") == "1"))


@app.route("/api/firmware", methods=["GET"])
def api_firmware_get():
    return jsonify({"ok": True, "firmware": fw.current()})


@app.route("/api/firmware", methods=["POST"])
def api_firmware_upload():
    """The raw firmware.bin as the request body (X-Filename names it)."""
    img = request.get_data(cache=False)
    try:
        meta = fw.save(img, request.headers.get("X-Filename", ""))
    except fw.FirmwareError as e:
        return jsonify({"ok": False, "error": str(e)}), 400
    emit("firmware", meta)
    return jsonify({"ok": True, "firmware": meta})


@app.route("/api/filters/validate", methods=["POST"])
def api_filters_validate():
    lst = (request.get_json(silent=True) or {}).get("filters") or []
    try:
        buf = flt.encode(lst)
        return jsonify({"ok": True, "crc": flt.crc(lst), "count": len(flt.decode(buf)), "bytes": len(buf)})
    except flt.FilterError as e:
        return jsonify({"ok": False, "error": str(e)})


@app.route("/api/settings", methods=["GET"])
def api_settings():
    return jsonify(public_settings())


def clean_tri(new, cur):
    """Merge and range-check the triangulation model (see DEFAULT_SETTINGS)."""
    if not isinstance(new, dict):
        raise ValueError("expected an object")
    out = {**DEFAULT_SETTINGS["tri"], **(cur or {})}
    clamp = {"p0_ble": (-90, -20), "p0_wifi": (-90, -10), "n": (1.6, 5.0), "smooth": (0.05, 1.0),
             "sigma": (1.5, 12.0), "max_err": (2.0, 30.0)}
    for k, (lo, hi) in clamp.items():
        if k in new:
            out[k] = round(min(hi, max(lo, float(new[k]))), 2)
    if "offsets" in new:
        offs = new["offsets"] or {}
        if not isinstance(offs, dict):
            raise ValueError("offsets must map node IDs to dB")
        out["offsets"] = {str(k).upper()[:6]: round(min(40.0, max(-40.0, float(v))), 1)
                          for k, v in offs.items() if re.fullmatch(r"[0-9A-Fa-f]{6}", str(k))}
    if "devices" in new:
        devs = new["devices"] or {}
        if not isinstance(devs, dict):
            raise ValueError("devices must map MACs to calibrations")
        out["devices"] = {}
        for mac, c in devs.items():
            mac = str(mac).upper()
            if not re.fullmatch(r"[0-9A-F]{2}(:[0-9A-F]{2}){5}", mac) or not isinstance(c, dict):
                continue
            out["devices"][mac] = {"p0": round(min(0.0, max(-100.0, float(c["p0"]))), 1),
                                   "n": round(min(5.0, max(1.6, float(c["n"]))), 2),
                                   "rms": round(min(30.0, max(0.0, float(c.get("rms", 0)))), 1),
                                   "nodes": int(c.get("nodes", 0)), "ts": float(c.get("ts", 0))}
    return out


@app.route("/api/settings", methods=["POST"])
def api_settings_set():
    data = request.get_json(silent=True) or {}
    restart = False
    if "status_poll" in data:
        data["status_poll"] = max(0, int(data["status_poll"] or 0))
    if "retention_days" in data:
        data["retention_days"] = max(0, int(data["retention_days"] or 0))
    for k in ("trusted_nets", "key_nets"):
        if k in data:
            nets, bad = parse_nets(data[k])
            if bad:
                return jsonify({"ok": False, "error": f"not a network (CIDR): {', '.join(bad)}"}), 400
            data[k] = ", ".join(str(n) for n in nets)
            if k == "key_nets" and not _is_local():
                return jsonify({"ok": False, "error": "who may view the fleet key can only be changed on this computer"}), 403
    for k in ("sound", "flash", "tiles", "tiles_custom", "map_view", "notify",
              "retention_days", "links", "traffic", "status_poll", "trusted_nets", "key_nets"):
        if k in data:
            store.set_setting(k, data[k])
    if "tri" in data:
        try:
            store.set_setting("tri", clean_tri(data["tri"], settings().get("tri") or {}))
        except (TypeError, ValueError) as e:
            return jsonify({"ok": False, "error": f"triangulation settings: {e}"}), 400
    if "serial_port" in data:
        store.set_setting("serial_port", str(data["serial_port"]) or "auto")
        restart = True
    if "mqtt" in data and isinstance(data["mqtt"], dict):
        cur = settings()["mqtt"]
        new = {**cur, **{k: v for k, v in data["mqtt"].items()
                         if k in ("enabled", "host", "port", "user", "password", "topic")}}
        if new.get("password") == "********":
            new["password"] = cur.get("password", "")
        new["port"] = int(new.get("port") or 1883)
        store.set_setting("mqtt", new)
        restart = True
    refresh_access()
    if restart and not offline_mode:
        start_sources()
    # Every open dashboard picks it up, except a bare map_view save: that is
    # this screen's pan/zoom (saved on every move), and broadcasting it made
    # every map reload its tiles, a black flash on each pan.
    if set(data) - {"map_view"}:
        emit("settings", public_settings())
    return jsonify(public_settings())


@app.route("/api/ports")
def api_ports():
    return jsonify(list_serial_ports())


@app.route("/api/console", methods=["POST"])
def api_console():
    line = str((request.get_json(silent=True) or {}).get("line", "")).strip()
    if not line:
        return jsonify({"ok": False, "error": "empty"}), 400
    with src_lock:
        ser = sources.get("serial")
    if not ser or not ser.send(line):
        return jsonify({"ok": False, "error": "serial not connected"}), 409
    fleet.feed_line(f"> {line}", "you")
    return jsonify({"ok": True})


@socketio.on("connect")
def on_connect():
    if not _authorized():
        return False   # refuse the websocket
    socketio.emit("sources", sources_status(), to=request.sid)


# ----------------------------------------------------------------------
# Bridge: a node on a Tab5 display's USB port, relayed over its Socket.IO link
# ----------------------------------------------------------------------
class BridgeSource(Source):
    """The home node plugged into a Tab5 instead of this computer. The Tab5 sends its
    serial lines up (bridge_lines) and console commands go back down (bridge_tx)."""
    kind = "bridge"

    def __init__(self, fleet, on_status, sid, dev):
        super().__init__(fleet, on_status)
        self.sid = sid
        self.node_id = None   # the node on the Tab5's USB, from its telemetry
        self.set_state(connected=True, detail=f"node on a Tab5 display (USB {dev})")

    def run(self):
        self.stop_evt.wait()
        self.set_state(connected=False, detail="stopped")

    def send(self, line):
        socketio.emit("bridge_tx", {"line": line.strip()}, to=self.sid)
        return True


def _bridge():
    with src_lock:
        b = sources.get("bridge")
    return b if b and b.sid == request.sid else None


@socketio.on("bridge_hello")
def on_bridge_hello(data):
    # Sent on connect and every 30 s while a node is plugged into the Tab5 (it also
    # brings the bridge back after start_sources() restarts the feeds).
    if _bridge():
        return
    dev = str((data or {}).get("dev", ""))[:16]
    src = BridgeSource(fleet, on_source_status, request.sid, dev)   # built outside the lock
    with src_lock:
        old = sources.pop("bridge", None)
        sources["bridge"] = src
    if old:
        old.stop()
    src.start()
    on_source_status()


@socketio.on("bridge_lines")
def on_bridge_lines(data):
    if not _bridge():
        return
    b = _bridge()
    for line in (data or {}).get("lines", [])[:256]:
        if isinstance(line, str) and line:
            if line.startswith("TLM "):
                m = re.search(r'"id":"([0-9A-F]{6})"', line)
                if m:
                    b.node_id = m.group(1)
            fleet.feed_line(line[:4096], "bridge")


@socketio.on("bridge_bye")
def on_bridge_bye(*_):
    _bridge_drop()


@socketio.on("disconnect")
def on_disconnect(*_):
    _bridge_drop()


def _bridge_drop():
    b = _bridge()
    if b:
        with src_lock:
            sources.pop("bridge", None)
        b.stop()
        on_source_status()


def _addresses():
    """This computer's network addresses, to print as ready-made links.
    Portable (Linux, macOS, Windows): no ifconfig/ip/ipconfig parsing. A UDP
    "connect" picks the outbound interface without sending anything."""
    out = []
    try:
        sk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sk.connect(("10.255.255.255", 1))          # private, unroutable: nothing is sent
        out.append(sk.getsockname()[0])
        sk.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            out.append(info[4][0])
    except OSError:
        pass
    seen = []
    for ip in out:
        try:
            a = ipaddress.ip_address(ip)
        except ValueError:
            continue
        if not a.is_loopback and ip not in seen:
            seen.append(ip)
    return seen


def advertise():
    """Announce the dashboard on the local network as _ouispy._tcp so wall displays
    (the Tab5 firmware) find it without an address typed in. LAN-only multicast DNS,
    with the generic instance name "OUI-SPY Mesh": no node IDs, no hostname, no token.
    Uses whatever the OS has (dns-sd on macOS, avahi on Linux, python-zeroconf if
    installed); quietly does nothing otherwise."""
    if args.no_advertise or args.host in ("127.0.0.1", "localhost"):
        return
    import shutil
    import subprocess
    name, kind, txt = "OUI-SPY Mesh", "_ouispy._tcp", "api=1"
    for cmd in (["dns-sd", "-R", name, kind, "local", str(args.port), txt],
                ["avahi-publish-service", name, kind, str(args.port), txt]):
        if shutil.which(cmd[0]):
            try:
                proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                import atexit
                atexit.register(proc.terminate)
                print(f"  mDNS           {kind} on port {args.port} (--no-advertise turns this off)")
            except OSError:
                pass
            return
    try:
        from zeroconf import ServiceInfo, Zeroconf
        ips = [socket.inet_aton(ip) for ip in _addresses()]
        if ips:
            zc = Zeroconf()
            zc.register_service(ServiceInfo(f"{kind}.local.", f"{name}.{kind}.local.", port=args.port,
                                            addresses=ips, properties={"api": "1"}))
            print(f"  mDNS           {kind} on port {args.port} (--no-advertise turns this off)")
    except Exception:
        pass


def _prewarm_tiles():
    """Fill the tile cache around the nodes (satellite, zoom 12-19) in the
    background, then again every 6 h in case a node moved: the Tab5 map
    opens on tiles that are already on this disk."""
    import tiles
    while True:
        if not args.demo:
            pts = [(m["lat"], m["lon"]) for m in store.nodes_meta().values()
                   if m.get("lat") is not None and m.get("lon") is not None]
            try:
                n = tiles.prewarm(pts)
                if n:
                    print(f"[tiles] cached {n} new tiles around the fleet")
            except Exception as e:
                print(f"[tiles] prewarm: {e}")
        time.sleep(6 * 3600)


if __name__ == "__main__":
    # kill -USR1 <pid> writes every thread's stack to the log: how to see a hang.
    import faulthandler
    import signal as _signal
    faulthandler.register(_signal.SIGUSR1, all_threads=True)
    refresh_access()
    start_sources()
    commander.start()
    threading.Thread(target=_prewarm_tiles, daemon=True, name="tile-prewarm").start()
    import lookups
    threading.Thread(target=lookups.ensure_data, daemon=True, name="lookup-data").start()
    import atexit
    import signal

    def _close_feeds(*_):
        # Close the serial port cleanly (one atomic line-state change) rather
        # than leaving it to process teardown, then exit.
        with src_lock:
            for src in sources.values():
                src.stop()
                ser = getattr(src, "ser", None)
                if ser:
                    try:
                        ser.close()
                    except Exception:
                        pass
    atexit.register(_close_feeds)
    signal.signal(signal.SIGTERM, lambda *a: (_close_feeds(), os._exit(0)))
    threading.Thread(target=ticker, daemon=True).start()
    mode = "DEMO" if args.demo else ("REPLAY" if args.replay else "LIVE")
    print(f"OUI-SPY Mesh [{mode}]  (db: {db_path})")
    print(f"  this computer:  http://127.0.0.1:{args.port}")
    if args.host not in ("127.0.0.1", "localhost"):
        q = f"/?token={ACCESS_TOKEN}" if args.require_token else ""
        for ip in _addresses():
            print(f"  network        http://{ip}:{args.port}{q}")
        if not args.require_token and not args.open:
            print(f"  (trusted networks open without a token; anything else needs ?token={ACCESS_TOKEN})")
    advertise()
    socketio.run(app, host=args.host, port=args.port, allow_unsafe_werkzeug=True)
