"""Fleet state + line parser for the OUI-SPY home node feed.

Every source (USB serial, MQTT, demo, replay) ends up calling one of:
  Fleet.feed_line(line, source)   raw serial console lines
  Fleet.feed_detection(obj, src)  detection JSON (serial line or MQTT payload)

Understood serial lines (firmware src/main.cpp + src/mesh.cpp):
  TLM {json}                      5 s telemetry: identity, counters, roster
  {"mac":..,"rssi":..,...}        one validated detection (MQTT payload too)
  [HEARTBEAT] ... lrssi=N         legacy counters (pre-TLM firmware)
  [MESH] ESP-NOW LR up: HOME node, channel 6, id A1B2C3
  [MESH] node D4E5F6 joined (remote) / offline / offline (bye)
  RST {json} / RFL {json}         a node's settings / filter list (remote mgmt)
  CMD / ACK / OK / ERR            command traffic, handed to the Commander
"""
import json
import re
import threading
import time

HOME_TIMEOUT = 15        # s without TLM/heartbeat -> home offline
NODE_STALE = 12          # s since last frame before a remote counts offline
SAMPLE_EVERY = 60        # s between per-node RSSI/pkt samples in history
DEDUP_WINDOW = 6         # s: same (node, mac, type) from two sources = one event

RE_HEARTBEAT = re.compile(r"\[HEARTBEAT\].*?\bmesh=(\d+) rx=(\d+) q=(\d+) dup=(\d+) "
                          r"self=(\d+) bad=(\d+) tx=(\d+)/(\d+)/(\d+).*?lrssi=(-?\d+)")
RE_HB_CH = re.compile(r"\bch=(\d+)")
RE_UP = re.compile(r"\[MESH\] ESP-NOW LR up: (HOME|REMOTE) node, channel (\d+), id ([0-9A-F]{6})")
RE_JOIN = re.compile(r"\[MESH\] node ([0-9A-F]{6}) joined(?: \((home|remote)\))?")
RE_OFF = re.compile(r"\[MESH\] node ([0-9A-F]{6}) offline")
RE_MESH_TAG = re.compile(r"\s*\[mesh ([0-9A-F]{6})\]\s*$")

HOME_STAT_KEYS = ("rx", "q", "dup", "self", "bad", "tx", "txf", "txcb", "pend",
                  "lrssi", "heal", "mqtt", "up", "heap", "dev", "filters", "n",
                  "mode", "mesh", "ch", "role",
                  # acked detection delivery (35d27bc+): sent, retries, acked, lost, in flight
                  "dsent", "dretry", "dack", "dlost", "dpend",
                  # radio duty, rolling 10 s: % parked on the mesh channel, % of it shared with BLE
                  "park", "pble",
                  # ESP-NOW TX rate in force (lr250k / lr500k / 1m / lr-failed) and stray sightings
                  "rate", "strays",
                  # fleet-key encryption on/off + frames rejected: plaintext, failed auth, replay
                  "enc", "eplain", "eauth", "ereplay")


class Fleet:
    def __init__(self, store, emit):
        self.store = store
        self.emit = emit                 # emit(event_name, payload)
        self.lock = threading.RLock()
        self.home_id = None
        self.home = {}                   # latest home stats
        self.home_seen = 0.0
        self.tlm_seen = 0.0              # last TLM line (firmware with telemetry)
        self.nodes = {}                  # id -> live state
        self.recent = {}                 # dedup key -> ts
        self.last_sample = {}
        self.legacy = False              # True once we parse pre-TLM firmware
        self.console = []                # ring of recent raw lines
        self.on_line = None              # Commander.feed_line, set by ouispymesh.py
        self.on_join = None              # Commander.node_joined
        meta = store.nodes_meta()
        for nid, m in meta.items():
            self.nodes[nid] = self._blank(nid, m.get("role"))
            self.nodes[nid]["last_seen"] = m.get("last_seen") or 0

    @staticmethod
    def _blank(nid, role=None):
        return {"id": nid, "role": role or "remote", "online": False, "last_seen": 0,
                "pkts": 0, "rssi": None, "since": None,
                "cfg": None, "cfg_ts": None, "filters": None, "filters_ok": True, "filters_ts": None}

    # ------------------------------------------------------------------
    # line ingestion
    # ------------------------------------------------------------------
    def feed_line(self, line, source="serial"):
        line = line.strip()
        if not line:
            return
        with self.lock:
            self.console.append([time.time(), source, line])
            del self.console[:-300]
        self.emit("console", {"ts": time.time(), "source": source, "line": line})
        if self.on_line and line.split(" ", 1)[0] in ("OK", "ERR", "CMD", "ACK", "RST", "RFL"):
            self.on_line(line)

        if line.startswith(("RST ", "RFL ")):
            try:
                obj = json.loads(line[4:])
            except ValueError:
                return
            if line.startswith("RST "):
                self._node_config(obj)
            else:
                self._node_filters(obj)
            return
        if line.startswith("TLM "):
            try:
                self.feed_tlm(json.loads(line[4:]), source)
            except ValueError:
                pass
            return
        body = line[4:] if line.startswith("DET ") else line
        if body.startswith("{") and '"mac"' in body:
            try:
                self.feed_detection(json.loads(body), source)
            except ValueError:
                pass
            return
        m = RE_HEARTBEAT.search(line)
        if m:
            self._legacy_heartbeat(m, line)
            return
        m = RE_UP.search(line)
        if m:
            if m.group(1) == "HOME":
                with self.lock:
                    self.home_id = m.group(3)
                    self.home["ch"] = int(m.group(2))
                self._mark_home_alive()
            return
        m = RE_JOIN.search(line)
        if m:
            self._node_seen(m.group(1), m.group(2), 0, None, None)
            self._broadcast()
            return
        m = RE_OFF.search(line)
        if m:
            self._node_offline(m.group(1))
            self._broadcast()

    def feed_tlm(self, t, source="serial"):
        now = time.time()
        with self.lock:
            if t.get("id"):
                self.home_id = t["id"]
            for k in HOME_STAT_KEYS:
                if k in t:
                    self.home[k] = t[k]
            self.home["source"] = source
            self.legacy = False
            self.tlm_seen = now
            seen = set()
            for n in t.get("nodes", []):
                nid = n.get("id")
                if not nid:
                    continue
                seen.add(nid)
                self._node_seen(nid, n.get("role"), n.get("last", 0), n.get("pkts"),
                                n.get("rssi") or None, now)
            # TLM is authoritative: anything not in the roster has dropped.
            for nid, st in list(self.nodes.items()):
                if st["online"] and nid not in seen and nid != self.home_id:
                    self._node_offline(nid)
        self._mark_home_alive(role=t.get("role"))
        self._broadcast()

    def _legacy_heartbeat(self, m, line):
        # TLM firmware prints [HEARTBEAT] too; TLM is the superset, so only
        # fall back to heartbeat parsing when no TLM has arrived lately.
        if time.time() - self.tlm_seen < HOME_TIMEOUT:
            return
        with self.lock:
            self.legacy = True
            keys = ("n", "rx", "q", "dup", "self", "bad", "tx", "txf", "txcb", "lrssi")
            for k, v in zip(keys, m.groups()):
                self.home[k] = int(v)
            self.home["mode"] = "scan"
            self.home["source"] = "serial"
        self._mark_home_alive()
        self._broadcast()

    # ------------------------------------------------------------------
    # detections
    # ------------------------------------------------------------------
    def feed_detection(self, d, source="serial"):
        now = time.time()
        mac = str(d.get("mac", "")).upper()
        if not mac:
            return
        descr = str(d.get("desc", ""))
        node = d.get("node")
        tag = RE_MESH_TAG.search(descr)
        if tag:
            node = node or tag.group(1)
            descr = descr[:tag.start()]
        node = (node or self.home_id or "HOME").upper()
        dom = d.get("dom")
        if dom not in ("ble", "wifi"):
            dom = "wifi" if str(d.get("type", "")).upper() in ("PROBE", "BEACON") else "ble"
        key = (node, mac, d.get("type"))
        with self.lock:
            for k, ts in list(self.recent.items()):
                if now - ts > DEDUP_WINDOW:
                    del self.recent[k]
            if key in self.recent:
                return
            self.recent[key] = now
        det = {
            "ts": now, "node": node, "mac": mac, "alias": d.get("alias") or "",
            "rssi": d.get("rssi"), "type": d.get("type"), "match": d.get("match"),
            "descr": descr.strip(), "dom": dom, "source": source,
            "is_new": not self.store.mac_known(mac),
        }
        det["id"] = self.store.add_detection(det)
        # A detection proves its node is alive even without telemetry.
        role = "home" if node == self.home_id else None
        self._node_seen(node, role, 0, None, None, now, from_det=True)
        self.emit("det", det)
        self._broadcast()

    # ------------------------------------------------------------------
    # node bookkeeping
    # ------------------------------------------------------------------
    def _node_seen(self, nid, role, last_s, pkts, rssi, now=None, from_det=False):
        now = now or time.time()
        with self.lock:
            st = self.nodes.get(nid)
            if st is None:
                st = self.nodes[nid] = self._blank(nid, role)
            if role in ("home", "remote"):
                st["role"] = role
            st["last_seen"] = max(st["last_seen"], now - (last_s or 0))
            if pkts is not None:
                st["pkts"] = pkts
            if rssi:
                st["rssi"] = rssi
            came_online = not st["online"]
            if came_online:
                st["online"] = True
                st["since"] = now
        self.store.touch_node(nid, st["role"], now)
        if came_online:
            self.store.add_sighting(nid, "online", st["rssi"], st["pkts"], now)
            self.emit("node_event", {"id": nid, "event": "online", "ts": now})
            if self.on_join:
                self.on_join(nid)
        if now - self.last_sample.get(nid, 0) >= SAMPLE_EVERY and not from_det:
            self.last_sample[nid] = now
            self.store.add_sighting(nid, "sample", st["rssi"], st["pkts"], now)

    def _node_config(self, cfg):
        nid = str(cfg.get("node", "")).upper()
        if len(nid) != 6:
            return
        now = time.time()
        with self.lock:
            st = self.nodes.get(nid)
            if st is None:
                st = self.nodes[nid] = self._blank(nid, cfg.get("role"))
            st["cfg"] = cfg
            st["cfg_ts"] = now
            if cfg.get("role") in ("home", "remote") and nid != self.home_id:
                st["role"] = cfg["role"]
        if nid != self.home_id:
            # A status reply is a frame from that node: it is alive.
            self._node_seen(nid, cfg.get("role"), 0, None, None, now, from_det=True)
        self._broadcast()

    def _node_filters(self, obj):
        nid = str(obj.get("node", "")).upper()
        if len(nid) != 6:
            return
        with self.lock:
            st = self.nodes.get(nid)
            if st is None:
                st = self.nodes[nid] = self._blank(nid)
            st["filters"] = obj.get("filters", [])
            st["filters_ok"] = bool(obj.get("ok", 1))
            st["filters_ts"] = time.time()
        self._broadcast()

    def _node_offline(self, nid):
        with self.lock:
            st = self.nodes.get(nid)
            if not st or not st["online"]:
                return
            st["online"] = False
            st["since"] = time.time()
        self.store.add_sighting(nid, "offline", st["rssi"], st["pkts"])
        self.emit("node_event", {"id": nid, "event": "offline", "ts": time.time()})

    def _mark_home_alive(self, role=None):
        now = time.time()
        with self.lock:
            self.home_seen = now
            hid = self.home_id
        if hid:
            self._node_seen(hid, "home" if role in (None, "home") else role, 0, None, None, now)

    def tick(self):
        """Called once a second: age out silent nodes and a silent home."""
        now = time.time()
        changed = False
        with self.lock:
            hid = self.home_id
            items = list(self.nodes.items())
        for nid, st in items:
            if not st["online"]:
                continue
            limit = HOME_TIMEOUT if nid == hid else NODE_STALE
            # Without TLM (legacy firmware / MQTT only) remotes are only
            # seen on detections, so give them a much longer leash.
            if nid != hid and (self.legacy or now - self.home_seen > HOME_TIMEOUT):
                limit = 600
            if now - st["last_seen"] > limit:
                self._node_offline(nid)
                changed = True
        if changed:
            self._broadcast()

    # ------------------------------------------------------------------
    # snapshots
    # ------------------------------------------------------------------
    def snapshot(self):
        meta = self.store.nodes_meta()
        now = time.time()
        with self.lock:
            nodes = []
            for nid, st in self.nodes.items():
                m = meta.get(nid, {})
                nodes.append({**st, "alias": m.get("alias") or "", "lat": m.get("lat"),
                              "lon": m.get("lon"), "notes": m.get("notes") or "",
                              "first_seen": m.get("first_seen"),
                              "is_home": nid == self.home_id,
                              "age": round(now - st["last_seen"], 1) if st["last_seen"] else None})
            home = dict(self.home)
            home.update({"id": self.home_id, "alive": now - self.home_seen < HOME_TIMEOUT,
                         "age": round(now - self.home_seen, 1) if self.home_seen else None,
                         "legacy": self.legacy})
        nodes.sort(key=lambda n: (not n["is_home"], not n["online"], n["id"]))
        return {"home": home, "nodes": nodes, "ts": now}

    def _broadcast(self):
        self.emit("fleet", self.snapshot())

    def console_tail(self):
        with self.lock:
            return list(self.console[-200:])
