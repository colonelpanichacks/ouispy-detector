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
import secrets
import socket
import threading
import time

from flask import Flask, Response, abort, jsonify, redirect, render_template, request
from flask_socketio import SocketIO

import filters as flt
from commands import Commander, PlanError
from fleet import Fleet
from sources import (DemoSource, MqttSource, ReplaySource, SerialSource,
                     list_serial_ports)
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
    ap.add_argument("--serial", help="serial port, 'auto' (USB VID 0x303A) or 'off'")
    ap.add_argument("--mqtt", help="MQTT broker host[:port] (enables MQTT)")
    ap.add_argument("--mqtt-user")
    ap.add_argument("--mqtt-pass")
    ap.add_argument("--topic", help="MQTT detection topic filter (default +/detection)")
    ap.add_argument("--demo", action="store_true", help="simulated fleet, separate database")
    ap.add_argument("--replay", help="replay a recorded serial log")
    ap.add_argument("--speed", type=float, default=1.0, help="replay speed factor")
    ap.add_argument("--record", help="append raw serial lines (with timestamps) to this file")
    ap.add_argument("--db", help="SQLite path (default ouispy.db / ouispy-demo.db)")
    return ap.parse_args()


args = parse_args()
offline_mode = bool(args.demo or args.replay)
db_path = args.db or os.path.join(HERE, "ouispy-demo.db" if offline_mode else "ouispy.db")

app = Flask(__name__)

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
sources = {}
src_lock = threading.Lock()


def command_sender():
    """The feed that can carry console commands: live serial, or the demo."""
    with src_lock:
        ser = sources.get("serial")
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
        new["demo"] = DemoSource(fleet, on_source_status)
        _seed_demo_positions()
    elif args.replay:
        new["replay"] = ReplaySource(fleet, on_source_status, args.replay, args.speed)
    else:
        port = args.serial or s["serial_port"]
        if port != "off":
            new["serial"] = SerialSource(fleet, on_source_status, port, record=args.record,
                                         get_pref=lambda: store.get_setting("home_usb_serial"),
                                         set_pref=lambda sn: store.set_setting("home_usb_serial", sn))
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
    base = (45.5231, -122.6765)
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
    from sources import DEMO_HOME, DEMO_REMOTES, DEMO_TARGETS
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
        mac = oui + ":" + ":".join(f"{random.randint(0, 255):02X}" for _ in range(3))
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
    if request.path.startswith("/api/"):
        resp.headers["Cache-Control"] = "no-store"
    return resp


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
    return jsonify({"fleet": fleet.snapshot(), "sources": sources_status(),
                    "settings": public_settings(), "stats": store.detection_stats(),
                    "hourly": store.hourly(), "mac_aliases": store.mac_aliases(),
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
    store.clear_detections()
    return jsonify({"ok": True})


@app.route("/api/stats")
def api_stats():
    return jsonify({**store.detection_stats(), "hourly": store.hourly()})


@app.route("/api/devices")
def api_devices():
    f = _filters()
    total, rows = store.devices(q=f["q"], node=f["node"], dom=f["dom"],
                                limit=min(int(request.args.get("limit", 300)), 2000))
    return jsonify({"total": total, "rows": rows})


@app.route("/api/devices/<mac>", methods=["POST"])
def api_device_alias(mac):
    data = request.get_json(silent=True) or {}
    mac = mac.upper()
    store.set_mac_alias(mac, str(data.get("alias") or "")[:40].strip(),
                        str(data.get("notes") or "")[:500] or None)
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
    store.update_node(nid.upper(), **fields)
    emit("fleet", fleet.snapshot())
    return jsonify({"ok": True})


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
    vendors = [{"key": g["key"], "label": g["label"], "kind": g["kind"], "count": len(g["ouis"]),
                "ouis": g["ouis"], "meta": g["meta"]} for g in flt.vendor_db()]
    return {"types": flt.LABELS, "hex": flt.HEX_LEN,
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
    return _submit(str(data.get("op", "")), nid.upper(), data.get("args") or {})


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
              "retention_days", "links", "status_poll", "trusted_nets", "key_nets"):
        if k in data:
            store.set_setting(k, data[k])
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


if __name__ == "__main__":
    refresh_access()
    start_sources()
    commander.start()
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
    socketio.run(app, host=args.host, port=args.port, allow_unsafe_werkzeug=True)
