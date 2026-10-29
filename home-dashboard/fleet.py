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

import filters as flt

HOME_TIMEOUT = 15        # s without TLM/heartbeat -> home offline
NODE_STALE = 12          # s since last frame before a remote counts offline
SAMPLE_EVERY = 60        # s between per-node RSSI/pkt samples in history
DEDUP_WINDOW = 6         # s: same (node, mac, type) from two sources = one event
SEEN_WINDOW = 60         # s: nodes that heard a device this recently count as "seen by"
BROADCAST_GAP = 0.25     # s: fleet updates are coalesced to <= 4 a second (first one at once)
FILTER_KEYS = ("filters", "filters_ok", "filters_ts", "filters_via")

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
                  "enc", "eplain", "eauth", "ereplay",
                  # free NVS entries (32 B each): low = settings writes start failing
                  "nvs")


def intel_name(intel):
    """The name a device gives itself: BLE local name, else an AP's SSID."""
    if not intel:
        return None
    if intel.get("name"):
        return intel["name"]
    if intel.get("frame") in ("beacon", "probe-resp") and intel.get("ssid"):
        return intel["ssid"]
    return None


class Fleet:
    def __init__(self, store, emit):
        self.store = store
        self.emit = emit                 # emit(event_name, payload)
        self.drones = None               # drones.Drones, set by the app
        self.tri = None                  # tri.Tri, set by the app: fixes for every display
        self.tri_fixes = {}              # mac -> last fix (sent with /api/state)
        self._tri_q = set()
        self._tri_ev = threading.Event()
        threading.Thread(target=self._tri_worker, daemon=True, name="tri").start()
        self.lock = threading.RLock()
        self.home_id = None
        self.home = {}                   # latest home stats
        self.home_seen = 0.0
        self.tlm_seen = 0.0              # last TLM line (firmware with telemetry)
        self.nodes = {}                  # id -> live state
        self.recent = {}                 # dedup key -> ts
        self.sightings = {}              # mac -> {node: (rssi, ts)}, last SEEN_WINDOW s
        self.last_sample = {}
        self.legacy = False              # True once we parse pre-TLM firmware
        self.console = []                # ring of recent raw lines
        self.on_line = None              # Commander.feed_line, set by ouispymesh.py
        self.on_join = None              # Commander.node_joined
        self.fcrc_hold = {}              # node or "all" -> time its fcrc is trusted again
        self._bc_lock = threading.Lock()  # fleet broadcasts: at most one per BROADCAST_GAP
        self._bc_timer = None
        self._bc_dirty = False
        meta = store.nodes_meta()
        for nid, m in meta.items():
            self.nodes[nid] = self._blank(nid, m.get("role"))
            self.nodes[nid]["last_seen"] = m.get("last_seen") or 0
        # Last firmware update per node (the live one comes from OTA lines).
        for nid, rec in (store.get_setting("ota_last") or {}).items():
            if nid in self.nodes:
                self.nodes[nid]["ota"] = rec
        # Last filter list read from each node, so a restart doesn't blank
        # every drawer (fcrc in the next RST says whether it is still current).
        for nid, rec in (store.get_setting("filter_lists") or {}).items():
            if nid in self.nodes and isinstance(rec.get("filters"), list):
                self.nodes[nid].update(filters=rec["filters"], filters_ts=rec.get("ts"),
                                       filters_via=rec.get("via"))

    @staticmethod
    def _blank(nid, role=None):
        return {"id": nid, "role": role or "remote", "online": False, "last_seen": 0,
                "pkts": 0, "rssi": None, "since": None,
                "cfg": None, "cfg_ts": None, "filters": None, "filters_ok": True, "filters_ts": None,
                "filters_via": None}

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
        if self.on_line and line.split(" ", 1)[0] in ("OK", "ERR", "CMD", "ACK", "RST", "RFL", "OTA"):
            self.on_line(line)
        if line.startswith("OTA "):
            self._ota_line(line)
            return

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
        if line.startswith("RID "):
            # Drone Remote ID from a mesh node (rid.cpp).
            if self.drones:
                try:
                    self.drones.feed(json.loads(line[4:]), "rid", self.home_id)
                except ValueError:
                    pass
            return
        if line.startswith("TLM "):
            try:
                self.feed_tlm(json.loads(line[4:]), source)
            except ValueError:
                pass
            return
        body = line[4:] if line.startswith("DET ") else line
        if body.startswith("{") and '"drone_lat"' in body:
            # A Sky-Spy board (mesh-mapper.py format) on this serial port.
            if self.drones:
                try:
                    self.drones.feed(json.loads(body), "skyspy", None)
                except ValueError:
                    pass
            return
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
        seen_by = self._sighting(mac, node, d.get("rssi"), now)
        if self.tri:
            self.tri.add(mac, node, d.get("rssi"), dom, now)
            with self.lock:
                self._tri_q.add(mac)
            self._tri_ev.set()
        # Device intel (name, services, SSIDs...) is merged from EVERY report,
        # signal updates and dedup repeats included: each can carry something new.
        info = d.get("info") if isinstance(d.get("info"), dict) else None
        name = None
        if info:
            try:
                intel = self.store.merge_intel(mac, info, node=node, rssi=d.get("rssi"), ts=now)
                name = intel_name(intel)
            except Exception as e:      # never lose the detection over intel
                print(f"[intel] {mac}: {e}")
        if name is None:
            rec = self.store.device_intel(mac)
            name = intel_name(rec["intel"]) if rec else None
        if d.get("upd"):
            # A signal update: the device is still in view of `node`. It moves
            # the live target and the "closest" call; no feed row, no alert.
            role = "home" if node == self.home_id else None
            self._node_seen(node, role, 0, None, None, now, from_det=True)
            self.emit("sighting", {"ts": now, "node": node, "mac": mac, "rssi": d.get("rssi"),
                                   "type": d.get("type"), "match": d.get("match"),
                                   "descr": descr.strip(), "dom": dom, "seen_by": seen_by,
                                   "name": name or "", "info": info})
            return
        with self.lock:
            for k, ts in list(self.recent.items()):
                if now - ts > DEDUP_WINDOW:
                    del self.recent[k]
            if key in self.recent:
                return
            self.recent[key] = now
        dash_alias = (self.store.mac_aliases().get(mac) or {}).get("alias") if hasattr(self.store, "mac_aliases") else None
        det = {
            "ts": now, "node": node, "mac": mac, "alias": dash_alias or d.get("alias") or "",
            "rssi": d.get("rssi"), "type": d.get("type"), "match": d.get("match"),
            "descr": descr.strip(), "dom": dom, "source": source,
            "name": name or "", "info": info,
            "group": self.store.group_of().get(mac) if hasattr(self.store, "group_of") else None,
            "is_new": not self.store.mac_known(mac),
            # Every node that heard this device lately, strongest first; with
            # two or more, the strongest is the one it is closest to.
            "seen_by": seen_by,
            "closest": seen_by[0][0] if len(seen_by) > 1 else None,
        }
        det["id"] = self.store.add_detection(det)
        # A detection proves its node is alive even without telemetry.
        role = "home" if node == self.home_id else None
        self._node_seen(node, role, 0, None, None, now, from_det=True)
        self.emit("det", det)
        self._broadcast()

    def _tri_worker(self):
        """Triangulate devices with new readings, at most once a second each, off the
        serial thread (a fix is a grid search: tens of ms)."""
        while True:
            self._tri_ev.wait(1.0)
            self._tri_ev.clear()
            with self.lock:
                macs, self._tri_q = self._tri_q, set()
            for mac in macs:
                if not self.tri or not self.tri.due(mac):
                    with self.lock:
                        self._tri_q.add(mac)   # try again next round
                    continue
                try:
                    fx = self.tri.fix(mac)
                except Exception as e:
                    print(f"[tri] {mac}: {e}")
                    continue
                fx["ts"] = time.time()
                self.tri_fixes[mac] = fx
                for m in [m for m, f in self.tri_fixes.items() if time.time() - f["ts"] > 600]:
                    del self.tri_fixes[m]
                self.emit("tri", fx)

    def _sighting(self, mac, node, rssi, now):
        """Record that `node` heard `mac` (repeats the dedup drops count too),
        and return [[node, rssi, age_s], ...] for the window, strongest first."""
        with self.lock:
            for m in [m for m, per in self.sightings.items()
                      if all(now - ts > SEEN_WINDOW for _, ts in per.values())]:
                del self.sightings[m]
            per = self.sightings.setdefault(mac, {})
            if isinstance(rssi, (int, float)):
                per[node] = (int(rssi), now)
            rows = [[n, r, round(now - ts)] for n, (r, ts) in per.items() if now - ts <= SEEN_WINDOW]
        rows.sort(key=lambda x: -x[1])
        return rows

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
            if cfg.get("role") == "home" and not self.home_id:
                # The home's own status (it answers on USB even when quiet,
                # e.g. no filters, no telemetry yet) names it as the home.
                self.home_id = nid
            if cfg.get("role") in ("home", "remote") and nid != self.home_id:
                st["role"] = cfg["role"]
        # The alias stored on the node (set by the web flasher) names it
        # here, unless one was already given on the dashboard: that wins.
        alias = str(cfg.get("alias") or "").strip()[:32]
        if alias and not (self.store.nodes_meta().get(nid) or {}).get("alias"):
            self.store.update_node(nid, alias=alias)
        if nid != self.home_id:
            # A status reply is a frame from that node: it is alive.
            self._node_seen(nid, cfg.get("role"), 0, None, None, now, from_det=True)
        self._broadcast()

    def _ota_line(self, line):
        """OTA {node, state, pct, ...} from the home: track each node's update,
        however it was started (dashboard, console, another browser), and
        keep the last result across restarts."""
        try:
            o = json.loads(line[4:])
        except ValueError:
            return
        nid = str(o.get("node", "")).upper()
        if len(nid) != 6:
            return
        now = time.time()
        with self.lock:
            st = self.nodes.get(nid)
            if st is None:
                return
            prev = st.get("ota") or {}
            started = prev.get("started") if prev.get("state") in ("offered", "sending") else now
            rec = {k: o.get(k) for k in ("state", "pct", "bytes", "size", "kbps", "direct", "proto", "err")}
            rec.update(ts=now, started=started or now)
            if rec["state"] in ("done", "failed"):
                rec["finished"] = now
            st["ota"] = rec
        if rec["state"] in ("done", "failed"):
            last = self.store.get_setting("ota_last") or {}
            last[nid] = rec
            self.store.set_setting("ota_last", last)
        self._broadcast()

    def _node_filters(self, obj, via=None):
        """A node's filter list: read from it (RFL), or `via` another node
        whose list has exactly the CRC this node reports."""
        nid = str(obj.get("node", "")).upper()
        if len(nid) != 6:
            return
        ok = bool(obj.get("ok", 1))
        with self.lock:
            st = self.nodes.get(nid)
            if st is None:
                st = self.nodes[nid] = self._blank(nid)
            st["filters"] = self._named(obj.get("filters", []))
            st["filters_ok"] = ok
            st["filters_ts"] = time.time()
            st["filters_via"] = via
            rec = {"filters": st["filters"], "ts": st["filters_ts"], "via": via}
        if ok:
            lists = self.store.get_setting("filter_lists") or {}
            lists[nid] = rec
            self.store.set_setting("filter_lists", lists)
        self.emit("node_filters", {"id": nid, "filters": rec["filters"], "filters_ok": ok,
                                   "filters_ts": rec["ts"], "filters_via": via})
        self._broadcast()

    def forget_fcrc(self, nid, hold=15):
        """A filter edit just went to `nid` (or "all"): the fcrc we hold is
        the old list's. A status asked for before the edit can still land
        after it carrying that old fcrc, so for `hold` seconds no fcrc of the
        node vouches for any list (reads go over the radio)."""
        with self.lock:
            self.fcrc_hold[nid] = time.time() + hold
            for n, st in self.nodes.items():
                if nid in ("all", n) and st.get("cfg"):
                    st["cfg"] = dict(st["cfg"], fcrc=None)

    def fcrc_trusted(self, nid):
        now = time.time()
        with self.lock:
            return all(self.fcrc_hold.get(k, 0) <= now for k in (nid, "all"))

    _lib_names = None

    @classmethod
    def _named(cls, filters):
        """A long list comes over the mesh without descriptions (it would not
        fit one transfer): name each row from the signature library."""
        if not any(not f.get("d") for f in filters):
            return filters
        if cls._lib_names is None:
            cls._lib_names = {}
            for g in flt.library():
                for r in g["rows"]:
                    try:
                        k = (r["t"], flt.decode(flt.encode([r]))[0]["id"].upper())
                    except Exception:
                        continue
                    cls._lib_names.setdefault(k, r.get("d") or g["label"])
        return [f if f.get("d") else {**f, "d": cls._lib_names.get((f.get("t"), str(f.get("id", "")).upper()), "")}
                for f in filters]

    _crc_cache = {}   # id(list) -> (list, crc): lists are replaced, never edited in place

    @classmethod
    def list_crc(cls, filters):
        """CRC of a list, computed once per list object (every broadcast and
        poll asks; a list only changes by being replaced)."""
        hit = cls._crc_cache.get(id(filters))
        if hit is not None and hit[0] is filters:
            return hit[1]
        try:
            c = flt.crc(filters)
        except Exception:
            c = None
        if len(cls._crc_cache) > 64:
            cls._crc_cache.clear()
        cls._crc_cache[id(filters)] = (filters, c)
        return c

    def filters_current(self, nid):
        """True when the list we hold for a node matches the fcrc it reports."""
        with self.lock:
            st = self.nodes.get(nid)
            if not st or st["filters"] is None or not st["filters_ok"]:
                return False
            fcrc = (st.get("cfg") or {}).get("fcrc")
            lst = st["filters"]
        return bool(fcrc) and self.list_crc(lst) == fcrc

    def filters_by_crc(self, nid):
        """A list held for any node whose CRC equals the fcrc `nid` reports:
        (filters, source node) or None. The CRC covers every byte the node
        stores (type, id, description), so a match is the same list."""
        with self.lock:
            fcrc = ((self.nodes.get(nid) or {}).get("cfg") or {}).get("fcrc")
            cands = [(n, st["filters"]) for n, st in self.nodes.items()
                     if st["filters"] is not None and st["filters_ok"]]
        if not fcrc or not self.fcrc_trusted(nid):
            return None
        # The node's own last read first, then the home, then anyone.
        cands.sort(key=lambda c: (c[0] != nid, c[0] != self.home_id))
        for src, lst in cands:
            if self.list_crc(lst) == fcrc:
                return [dict(f) for f in lst], src
        return None

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
    def snapshot(self, lists=True):
        """Whole fleet state. lists=False leaves out the per-node filter
        lists (the bulk of it, and they rarely change): live updates carry
        those in their own node_filters event, only when a list changes."""
        meta = self.store.nodes_meta()
        now = time.time()
        with self.lock:
            nodes = []
            for nid, st in self.nodes.items():
                m = meta.get(nid, {})
                nodes.append({**st, "alias": m.get("alias") or "", "lat": m.get("lat"),
                              "lon": m.get("lon"), "notes": m.get("notes") or "",
                              "color": m.get("color"),
                              "first_seen": m.get("first_seen"),
                              "is_home": nid == self.home_id,
                              "age": round(now - st["last_seen"], 1) if st["last_seen"] else None})
            home = dict(self.home)
            home.update({"id": self.home_id, "alive": now - self.home_seen < HOME_TIMEOUT,
                         "age": round(now - self.home_seen, 1) if self.home_seen else None,
                         "legacy": self.legacy})
        for n in nodes:
            # Does the list we show still match the fcrc the node reports?
            fcrc = (n.get("cfg") or {}).get("fcrc")
            n["filters_fresh"] = (None if n["filters"] is None or not fcrc
                                  else self.list_crc(n["filters"]) == fcrc)
        if not lists:
            for n in nodes:
                for k in FILTER_KEYS:
                    n.pop(k, None)
        nodes.sort(key=lambda n: (not n["is_home"], not n["online"], n["id"]))
        return {"home": home, "nodes": nodes, "ts": now}

    def _broadcast(self):
        """Push the fleet to every dashboard: the first change goes out at
        once, a burst behind it (a detection storm, a status flood) is folded
        into one update at the end of BROADCAST_GAP, always the latest state."""
        with self._bc_lock:
            if self._bc_timer is not None:
                self._bc_dirty = True
                return
            self._bc_timer = threading.Timer(BROADCAST_GAP, self._broadcast_tail)
            self._bc_timer.daemon = True
            self._bc_timer.start()
        self.emit("fleet", self.snapshot(lists=False))

    def _broadcast_tail(self):
        with self._bc_lock:
            dirty, self._bc_dirty, self._bc_timer = self._bc_dirty, False, None
        if dirty:
            self._broadcast()

    def console_tail(self):
        with self.lock:
            return list(self.console[-200:])
