"""Wardriving: every access point and Bluetooth device a wardriver node heard (the
`wardrive` firmware build, src/wardrive.h), WiGLE style.

A wardriver prints "WD {json}" lines. A Tab5 with a GPS stamps them with its fix
("lat", "lon", "acc") before it keeps them and bridges them here; lines without a fix
are still counted, they just can't go on the map.

Kept per device (MAC + radio): name / SSID, security, channel, maker, the strongest
signal and where it was heard (WiGLE places a device where it was loudest), first and
last seen, sightings, and the session it was last seen in. Every positioned sighting is
logged too, for the WiGLE CSV export.

Sessions: a new one starts after SESSION_GAP_S without wardrive data, or on request
(POST /api/wardrive/session). The maps show this session's devices or the whole history.
Writes are batched once a second on a worker thread, and the batch goes out to the
browsers and the Tab5 as one "wd" event.
"""

import json
import queue
import threading
import time

import lookups
from lookups import oui_vendor

# Intel a WD line may carry, merged per device (src/wardrive.cpp).
INTEL_KEYS = {"wifi": ("std", "rate", "cc", "vend", "hidden", "wps"),
              "ble": ("mfr", "app", "tx", "fl", "svc", "rnd")}
AD_FLAGS = [(0x01, "LE Limited Discoverable"), (0x02, "LE General Discoverable"), (0x04, "BR/EDR not supported"),
            (0x08, "LE + BR/EDR (controller)"), (0x10, "LE + BR/EDR (host)")]

SESSION_GAP_S = 1800

SCHEMA = """
CREATE TABLE IF NOT EXISTS wd_sessions (
    id INTEGER PRIMARY KEY, start REAL, last REAL, note TEXT);
CREATE TABLE IF NOT EXISTS wd_devices (
    mac TEXT NOT NULL, kind TEXT NOT NULL, name TEXT, sec TEXT, ch INTEGER, mfr INTEGER,
    app INTEGER, rnd INTEGER, rssi INTEGER, lat REAL, lon REAL, acc REAL,
    first REAL, last REAL, seen INTEGER, session INTEGER, node TEXT,
    PRIMARY KEY (mac, kind));
CREATE INDEX IF NOT EXISTS wd_devices_session ON wd_devices(session);
CREATE TABLE IF NOT EXISTS wd_obs (
    ts REAL, mac TEXT, kind TEXT, rssi INTEGER, lat REAL, lon REAL, acc REAL, session INTEGER);
CREATE INDEX IF NOT EXISTS wd_obs_session ON wd_obs(session);
"""


def _num(v, lo=None, hi=None):
    try:
        x = float(v)
    except (TypeError, ValueError):
        return None
    if x != x or (lo is not None and x < lo) or (hi is not None and x > hi):
        return None
    return x


class Wardrive:
    def __init__(self, store, emit):
        self.store = store
        self.emit = emit
        self.q = queue.Queue(maxsize=20000)
        self.lock = threading.RLock()   # _write -> _session_for -> new_session takes it again
        self.session = None
        self.session_last = 0.0
        with store.lock:
            store.db.executescript(SCHEMA)
            if "info" not in {r[1] for r in store.db.execute("PRAGMA table_info(wd_devices)")}:
                store.db.execute("ALTER TABLE wd_devices ADD COLUMN info TEXT")   # JSON intel, merged
            row = store.db.execute("SELECT id, last FROM wd_sessions ORDER BY id DESC LIMIT 1").fetchone()
            store.db.commit()
        if row:
            self.session, self.session_last = row["id"], row["last"] or 0.0
        threading.Thread(target=self._worker, name="wardrive", daemon=True).start()

    # ------------------------------------------------------------------ input
    def feed(self, obj, node=None):
        """One WD line (already JSON-decoded)."""
        kind = obj.get("t")
        mac = str(obj.get("mac") or "").upper()
        if kind not in ("wifi", "ble") or len(mac) != 17:
            return
        try:
            self.q.put_nowait((time.time(), kind, mac, obj, node))
        except queue.Full:
            pass

    def new_session(self, note=""):
        with self.lock:
            now = time.time()
            with self.store.lock:
                cur = self.store.db.execute("INSERT INTO wd_sessions(start, last, note) VALUES(?,?,?)",
                                            (now, now, note[:80]))
                self.store.db.commit()
            self.session, self.session_last = cur.lastrowid, now
        self.emit("wd_session", {"session": self.session})
        return self.session

    def _session_for(self, now):
        if self.session is None or now - self.session_last > SESSION_GAP_S:
            self.new_session()
        self.session_last = now
        return self.session

    # ------------------------------------------------------------------ worker
    def _worker(self):
        while True:
            batch = [self.q.get()]
            deadline = time.time() + 1.0
            while time.time() < deadline and len(batch) < 2000:
                try:
                    batch.append(self.q.get(timeout=max(0.0, deadline - time.time())))
                except queue.Empty:
                    break
            try:
                self._write(batch)
            except Exception as e:   # never let one bad batch stop wardriving
                print("wardrive: batch failed:", e)

    def _write(self, batch):
        out = {}
        with self.lock:
            sess = self._session_for(batch[-1][0])
        with self.store.lock:
            db = self.store.db
            for ts, kind, mac, o, node in batch:
                rssi = _num(o.get("rssi"), -127, 20)
                lat, lon = _num(o.get("lat"), -90, 90), _num(o.get("lon"), -180, 180)
                if lat is not None and lon is not None and lat == 0 and lon == 0:
                    lat = lon = None
                acc = _num(o.get("acc"), 0, 100000)
                name = str(o.get("ssid") if kind == "wifi" else o.get("name") or "")[:64]
                row = db.execute("SELECT * FROM wd_devices WHERE mac=? AND kind=?", (mac, kind)).fetchone()
                intel = {}
                if row is not None and row["info"]:
                    try:
                        intel = json.loads(row["info"])
                    except ValueError:
                        intel = {}
                for k in INTEL_KEYS[kind]:
                    if o.get(k) not in (None, "", []):
                        intel[k] = o[k]
                if kind == "ble" and o.get("svc"):   # keep every service UUID ever advertised
                    intel["svc"] = sorted(set(intel.get("svc_all", [])) | set(o["svc"]))
                    intel["svc_all"] = intel["svc"]
                info = json.dumps(intel) if intel else None
                if row is None:
                    db.execute(
                        "INSERT INTO wd_devices(mac, kind, name, sec, ch, mfr, app, rnd, rssi, lat, lon, acc,"
                        " first, last, seen, session, node, info) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,?,?,?)",
                        (mac, kind, name, o.get("sec") or None, o.get("ch"), o.get("mfr"), o.get("app"),
                         1 if o.get("rnd") else 0, rssi, lat, lon, acc, ts, ts, sess, node, info))
                else:
                    # Position: where it was loudest (with a fix).
                    better = lat is not None and (row["lat"] is None or (rssi is not None and
                                                  (row["rssi"] is None or rssi > row["rssi"])))
                    db.execute(
                        "UPDATE wd_devices SET name=COALESCE(NULLIF(?,''), name), sec=COALESCE(?, sec),"
                        " ch=COALESCE(?, ch), mfr=COALESCE(?, mfr), app=COALESCE(?, app), last=?,"
                        " seen=seen+1, session=?, node=COALESCE(?, node),"
                        " rssi=CASE WHEN ? THEN ? ELSE MAX(COALESCE(rssi,-127), COALESCE(?,-127)) END,"
                        " lat=CASE WHEN ? THEN ? ELSE lat END, lon=CASE WHEN ? THEN ? ELSE lon END,"
                        " acc=CASE WHEN ? THEN ? ELSE acc END, info=COALESCE(?, info) WHERE mac=? AND kind=?",
                        (name, o.get("sec") or None, o.get("ch"), o.get("mfr"), o.get("app"), ts, sess, node,
                         better, rssi, rssi, better, lat, better, lon, better, acc, info, mac, kind))
                if lat is not None:
                    db.execute("INSERT INTO wd_obs(ts, mac, kind, rssi, lat, lon, acc, session) VALUES(?,?,?,?,?,?,?,?)",
                               (ts, mac, kind, rssi, lat, lon, acc, sess))
                out[(mac, kind)] = None
            db.commit()
            rows = [db.execute("SELECT * FROM wd_devices WHERE mac=? AND kind=?", k).fetchone() for k in out]
        # Small events: a Tab5 takes each WebSocket message whole into internal RAM, and
        # one batch of a busy drive can be hundreds of devices.
        pub = [self._public(r, sess) for r in rows if r]
        for i in range(0, len(pub), 20):
            self.emit("wd", {"session": sess, "devices": pub[i:i + 20]})

    # ------------------------------------------------------------------ output
    def _public(self, r, sess=None):
        sess = self.session if sess is None else sess
        d = {k: r[k] for k in ("mac", "kind", "name", "sec", "ch", "mfr", "app", "rssi", "lat", "lon", "acc",
                               "first", "last", "seen", "node")}
        d["rnd"] = bool(r["rnd"])
        d["now"] = r["session"] == sess   # seen this session
        # Maker from the MAC: real for WiFi and public BLE addresses, meaningless for random ones.
        d["vendor"] = None if d["rnd"] else (oui_vendor(r["mac"]) or None)
        return d

    def node_ids(self):
        """Every node that has wardriven anything (known before its status arrives)."""
        with self.store.lock:
            return [r[0] for r in self.store.db.execute("SELECT DISTINCT node FROM wd_devices WHERE node IS NOT NULL")]

    def detail(self, mac, kind):
        """Everything known about one wardriven device, decoded (names for company IDs,
        service UUIDs, vendor IEs, appearance, AD flags), plus where it was heard."""
        mac = mac.upper()
        with self.store.lock:
            r = self.store.db.execute("SELECT * FROM wd_devices WHERE mac=? AND kind=?", (mac, kind)).fetchone()
            if not r:
                return None
            obs = self.store.db.execute("SELECT ts, rssi, lat, lon, acc, session FROM wd_obs WHERE mac=? AND kind=?"
                                        " ORDER BY ts DESC LIMIT 300", (mac, kind)).fetchall()
            sessions = self.store.db.execute("SELECT COUNT(DISTINCT session) n FROM wd_obs WHERE mac=? AND kind=?",
                                             (mac, kind)).fetchone()["n"]
        d = self._public(r)
        try:
            intel = json.loads(r["info"] or "{}")
        except ValueError:
            intel = {}
        intel.pop("svc_all", None)
        d["intel"] = intel
        mk = lookups.mac_kind(mac)
        dec = {"address": ("random (" + (mk.get("ble_random_kind") or "private") + ")") if d["rnd"] or mk.get("local")
               else "public (globally unique)"}
        if kind == "wifi":
            dec["standard"] = {"ax": "WiFi 6 (802.11ax)", "ac": "WiFi 5 (802.11ac)", "n": "WiFi 4 (802.11n)"}.get(
                intel.get("std"), "802.11a/b/g")
            if intel.get("rate"):
                dec["top legacy rate"] = f"{intel['rate']} Mb/s"
            if intel.get("cc"):
                dec["country"] = intel["cc"]
            if intel.get("vend"):
                dec["vendor tags"] = [{"oui": v, "name": lookups.vie_vendor(v) or "unknown"} for v in intel["vend"]]
            dec["hidden"] = bool(intel.get("hidden"))
            dec["WPS"] = bool(intel.get("wps"))
        else:
            if d.get("mfr") is not None:
                dec["company"] = {"id": f"0x{int(d['mfr']):04X}", "name": lookups.company(f"{int(d['mfr']):04x}") or "unknown"}
            if d.get("app"):
                dec["appearance"] = lookups.appearance(d["app"]) or str(d["app"])
            if intel.get("svc"):
                dec["services"] = [{"uuid": u, "name": lookups.uuid16(u) or "unknown"} for u in intel["svc"]]
            if intel.get("tx") is not None:
                dec["tx power"] = f"{intel['tx']} dBm"
            if intel.get("fl"):
                dec["flags"] = [n for b, n in AD_FLAGS if intel["fl"] & b]
        d["decoded"] = dec
        d["sightings"] = [dict(o) for o in obs]
        d["sessions"] = sessions
        return d

    def devices(self, scope="session", limit=20000, placed=False, node=None):
        where = ["lat IS NOT NULL"] if placed else []
        args = []
        if node:
            where.append("node=?")
            args.append(node)
        if scope != "history":
            where.append("session=?")
            args.append(self.session or -1)
        q = "SELECT * FROM wd_devices" + (" WHERE " + " AND ".join(where) if where else "") + " ORDER BY last DESC LIMIT ?"
        with self.store.lock:
            rows = self.store.db.execute(q, (*args, limit)).fetchall()
        return [self._public(r) for r in rows]

    def summary(self):
        with self.store.lock:
            db = self.store.db
            tot = db.execute("SELECT kind, COUNT(*) n, SUM(lat IS NOT NULL) p FROM wd_devices GROUP BY kind").fetchall()
            ses = db.execute("SELECT kind, COUNT(*) n FROM wd_devices WHERE session=? GROUP BY kind",
                             (self.session or -1,)).fetchall()
            srow = db.execute("SELECT * FROM wd_sessions WHERE id=?", (self.session or -1,)).fetchone()
        return {"session": self.session, "session_start": srow["start"] if srow else None,
                "history": {r["kind"]: {"n": r["n"], "placed": r["p"] or 0} for r in tot},
                "this_session": {r["kind"]: r["n"] for r in ses}}

    def wigle_csv(self, scope="session"):
        """WiGLE CSV (format 1.4): one row per positioned sighting."""
        head = ("WigleWifi-1.4,appRelease=1.0,model=OUI-SPY Mesh,release=1.0,device=OUI-SPY wardriver,"
                "display=,board=,brand=colonelpanic.tech\n"
                "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,"
                "AltitudeMeters,AccuracyMeters,Type\n")
        q = ("SELECT o.ts, o.mac, o.kind, o.rssi, o.lat, o.lon, o.acc, d.name, d.sec, d.ch FROM wd_obs o"
             " LEFT JOIN wd_devices d ON d.mac=o.mac AND d.kind=o.kind")
        args = ()
        if scope != "history":
            q += " WHERE o.session=?"
            args = (self.session or -1,)
        q += " ORDER BY o.ts"
        auth = {"open": "[ESS]", "WEP": "[WEP][ESS]", "WPA": "[WPA-PSK-CCMP][ESS]", "WPA2": "[WPA2-PSK-CCMP][ESS]",
                "WPA3": "[WPA3-SAE-CCMP][ESS]", "WPA2/WPA3": "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]"}

        def esc(s):
            s = str(s or "")
            return '"' + s.replace('"', '""') + '"' if any(c in s for c in ',"\n') else s

        yield head
        with self.store.lock:
            rows = self.store.db.execute(q, args).fetchall()
        for r in rows:
            t = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(r["ts"]))
            if r["kind"] == "wifi":
                yield (f'{r["mac"]},{esc(r["name"])},{auth.get(r["sec"] or "", "[ESS]")},{t},{r["ch"] or ""},'
                       f'{r["rssi"] if r["rssi"] is not None else ""},{r["lat"]:.7f},{r["lon"]:.7f},0,'
                       f'{r["acc"] or 0:.1f},WIFI\n')
            else:
                yield (f'{r["mac"]},{esc(r["name"])},Misc [LE],{t},0,'
                       f'{r["rssi"] if r["rssi"] is not None else ""},{r["lat"]:.7f},{r["lon"]:.7f},0,'
                       f'{r["acc"] or 0:.1f},BLE\n')
