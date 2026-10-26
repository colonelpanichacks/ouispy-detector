"""SQLite persistence: detections, node sightings, node metadata, settings.

Everything stays on this machine. One connection shared across threads,
serialized by a lock (writes are tiny and infrequent).
"""
import csv
import io
import json
import sqlite3
import threading
import time

SCHEMA = """
CREATE TABLE IF NOT EXISTS detections (
    id      INTEGER PRIMARY KEY AUTOINCREMENT,
    ts      REAL NOT NULL,
    node    TEXT NOT NULL,
    mac     TEXT NOT NULL,
    alias   TEXT,
    rssi    INTEGER,
    type    TEXT,
    match   TEXT,
    descr   TEXT,
    dom     TEXT,
    source  TEXT,
    is_new  INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS det_ts   ON detections(ts);
CREATE INDEX IF NOT EXISTS det_node ON detections(node);
CREATE INDEX IF NOT EXISTS det_mac  ON detections(mac);

CREATE TABLE IF NOT EXISTS sightings (
    id     INTEGER PRIMARY KEY AUTOINCREMENT,
    ts     REAL NOT NULL,
    node   TEXT NOT NULL,
    event  TEXT NOT NULL,      -- online | offline | sample
    rssi   INTEGER,
    pkts   INTEGER
);
CREATE INDEX IF NOT EXISTS sig_node_ts ON sightings(node, ts);

CREATE TABLE IF NOT EXISTS nodes (
    id         TEXT PRIMARY KEY,
    alias      TEXT,
    lat        REAL,
    lon        REAL,
    notes      TEXT,
    role       TEXT,
    first_seen REAL,
    last_seen  REAL
);

CREATE TABLE IF NOT EXISTS mac_aliases (
    mac   TEXT PRIMARY KEY,
    alias TEXT,
    notes TEXT
);

CREATE TABLE IF NOT EXISTS commands (
    id       INTEGER PRIMARY KEY,
    ts       REAL NOT NULL,
    op       TEXT,
    target   TEXT,
    label    TEXT,
    state    TEXT,
    cmd_id   INTEGER,
    replies  TEXT,
    error    TEXT,
    finished REAL,
    auto     INTEGER
);
CREATE INDEX IF NOT EXISTS cmd_ts ON commands(ts);

CREATE TABLE IF NOT EXISTS tracks (
    id    INTEGER PRIMARY KEY AUTOINCREMENT,
    ts    REAL NOT NULL,
    node  TEXT NOT NULL,
    lat   REAL NOT NULL,
    lon   REAL NOT NULL,
    acc   REAL
);
CREATE INDEX IF NOT EXISTS trk_node_ts ON tracks(node, ts);

CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT
);
"""

DET_COLS = ["id", "ts", "node", "mac", "alias", "rssi", "type", "match",
            "descr", "dom", "source", "is_new"]


class Store:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.executescript(SCHEMA)
        self.db.commit()

    # ---------- settings ----------
    def get_setting(self, key, default=None):
        with self.lock:
            row = self.db.execute("SELECT value FROM settings WHERE key=?", (key,)).fetchone()
        if row is None:
            return default
        try:
            return json.loads(row["value"])
        except ValueError:
            return default

    def set_setting(self, key, value):
        with self.lock:
            self.db.execute("INSERT INTO settings(key,value) VALUES(?,?) "
                            "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                            (key, json.dumps(value)))
            self.db.commit()

    def all_settings(self):
        with self.lock:
            rows = self.db.execute("SELECT key, value FROM settings").fetchall()
        out = {}
        for r in rows:
            try:
                out[r["key"]] = json.loads(r["value"])
            except ValueError:
                pass
        return out

    # ---------- nodes ----------
    def nodes_meta(self):
        with self.lock:
            rows = self.db.execute("SELECT * FROM nodes").fetchall()
        return {r["id"]: dict(r) for r in rows}

    def touch_node(self, node_id, role=None, ts=None):
        ts = ts or time.time()
        with self.lock:
            self.db.execute(
                "INSERT INTO nodes(id, role, first_seen, last_seen) VALUES(?,?,?,?) "
                "ON CONFLICT(id) DO UPDATE SET last_seen=excluded.last_seen, "
                "role=COALESCE(excluded.role, nodes.role)",
                (node_id, role, ts, ts))
            self.db.commit()

    def update_node(self, node_id, **fields):
        allowed = {k: v for k, v in fields.items() if k in ("alias", "lat", "lon", "notes")}
        if not allowed:
            return
        with self.lock:
            self.db.execute("INSERT OR IGNORE INTO nodes(id, first_seen) VALUES(?, ?)",
                            (node_id, time.time()))
            sets = ", ".join(f"{k}=?" for k in allowed)
            self.db.execute(f"UPDATE nodes SET {sets} WHERE id=?",
                            (*allowed.values(), node_id))
            self.db.commit()

    def forget_node(self, node_id):
        with self.lock:
            self.db.execute("DELETE FROM nodes WHERE id=?", (node_id,))
            self.db.commit()

    # ---------- detections ----------
    def mac_known(self, mac):
        with self.lock:
            return self.db.execute("SELECT 1 FROM detections WHERE mac=? LIMIT 1",
                                   (mac,)).fetchone() is not None

    def add_detection(self, d):
        with self.lock:
            cur = self.db.execute(
                "INSERT INTO detections(ts,node,mac,alias,rssi,type,match,descr,dom,source,is_new) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                (d["ts"], d["node"], d["mac"], d.get("alias"), d.get("rssi"), d.get("type"),
                 d.get("match"), d.get("descr"), d.get("dom"), d.get("source"),
                 1 if d.get("is_new") else 0))
            self.db.commit()
            return cur.lastrowid

    def _det_where(self, q=None, node=None, dom=None, since=None, until=None, mac=None):
        where, args = [], []
        if q:
            where.append("(mac LIKE ? OR alias LIKE ? OR descr LIKE ? OR match LIKE ? OR type LIKE ? "
                         "OR mac IN (SELECT mac FROM mac_aliases WHERE alias LIKE ?))")
            like = f"%{q}%"
            args += [like] * 6
        if node:
            where.append("node=?")
            args.append(node)
        if mac:
            where.append("mac=?")
            args.append(mac)
        if dom in ("ble", "wifi"):
            where.append("dom=?")
            args.append(dom)
        if since:
            where.append("ts>=?")
            args.append(float(since))
        if until:
            where.append("ts<=?")
            args.append(float(until))
        return (" WHERE " + " AND ".join(where)) if where else "", args

    def query_detections(self, limit=200, offset=0, **filters):
        w, args = self._det_where(**filters)
        with self.lock:
            total = self.db.execute(f"SELECT COUNT(*) FROM detections{w}", args).fetchone()[0]
            rows = self.db.execute(
                f"SELECT * FROM detections{w} ORDER BY ts DESC LIMIT ? OFFSET ?",
                (*args, int(limit), int(offset))).fetchall()
        return total, [dict(r) for r in rows]

    def export_detections(self, fmt, **filters):
        w, args = self._det_where(**filters)
        with self.lock:
            rows = [dict(r) for r in self.db.execute(
                f"SELECT *, (SELECT alias FROM mac_aliases m WHERE m.mac = detections.mac) mac_alias "
                f"FROM detections{w} ORDER BY ts ASC", args).fetchall()]
        for r in rows:
            r["time"] = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(r["ts"]))
        if fmt == "json":
            return json.dumps(rows, indent=1)
        buf = io.StringIO()
        cols = ["time"] + DET_COLS + ["mac_alias"]
        wr = csv.DictWriter(buf, fieldnames=cols, extrasaction="ignore")
        wr.writeheader()
        wr.writerows(rows)
        return buf.getvalue()

    def detection_stats(self):
        since = time.time() - 86400
        with self.lock:
            total = self.db.execute("SELECT COUNT(*) FROM detections").fetchone()[0]
            day = self.db.execute("SELECT COUNT(*) FROM detections WHERE ts>=?",
                                  (since,)).fetchone()[0]
            macs = self.db.execute("SELECT COUNT(DISTINCT mac) FROM detections").fetchone()[0]
            per_node = self.db.execute(
                "SELECT node, COUNT(*) c, MAX(ts) last FROM detections GROUP BY node").fetchall()
        return {"total": total, "day": day, "macs": macs,
                "per_node": {r["node"]: {"count": r["c"], "last": r["last"]} for r in per_node}}

    def hourly(self, hours=24):
        """Detections per hour for the last `hours` hours, oldest first."""
        now = time.time()
        start = (int(now) // 3600 - hours + 1) * 3600
        with self.lock:
            rows = self.db.execute(
                "SELECT CAST(ts / 3600 AS INTEGER) h, COUNT(*) c FROM detections "
                "WHERE ts >= ? GROUP BY h", (start,)).fetchall()
        got = {r["h"]: r["c"] for r in rows}
        first = start // 3600
        return [{"h": (first + i) * 3600, "c": got.get(first + i, 0)} for i in range(hours)]

    def devices(self, q=None, node=None, dom=None, limit=300):
        """One row per MAC: what it is, when, how often, which nodes saw it."""
        w, args = self._det_where(q=q, node=node, dom=dom)
        with self.lock:
            rows = self.db.execute(
                f"SELECT mac, MAX(descr) descr, MAX(type) type, MAX(dom) dom, COUNT(*) hits, "
                f"MIN(ts) first, MAX(ts) last, MAX(rssi) best, "
                f"GROUP_CONCAT(DISTINCT node) nodes FROM detections{w} "
                f"GROUP BY mac ORDER BY last DESC LIMIT ?", (*args, int(limit))).fetchall()
            total = self.db.execute(
                f"SELECT COUNT(DISTINCT mac) FROM detections{w}", args).fetchone()[0]
        out = []
        for r in rows:
            d = dict(r)
            d["nodes"] = sorted(set((d["nodes"] or "").split(","))) if d["nodes"] else []
            out.append(d)
        return total, out

    def mac_aliases(self):
        with self.lock:
            rows = self.db.execute("SELECT mac, alias, notes FROM mac_aliases").fetchall()
        return {r["mac"]: {"alias": r["alias"] or "", "notes": r["notes"] or ""} for r in rows}

    def set_mac_alias(self, mac, alias, notes=None):
        with self.lock:
            if not alias and not notes:
                self.db.execute("DELETE FROM mac_aliases WHERE mac=?", (mac,))
            else:
                self.db.execute("INSERT INTO mac_aliases(mac, alias, notes) VALUES(?,?,?) "
                                "ON CONFLICT(mac) DO UPDATE SET alias=excluded.alias, "
                                "notes=excluded.notes", (mac, alias, notes))
            self.db.commit()

    def prune(self, days):
        """Drop detections and sightings older than `days` (0 = keep forever)."""
        if not days or days <= 0:
            return 0
        cutoff = time.time() - days * 86400
        with self.lock:
            n = self.db.execute("DELETE FROM detections WHERE ts < ?", (cutoff,)).rowcount
            n += self.db.execute("DELETE FROM sightings WHERE ts < ?", (cutoff,)).rowcount
            n += self.db.execute("DELETE FROM tracks WHERE ts < ?", (cutoff,)).rowcount
            self.db.commit()
        return n

    # ---------- remote command log ----------
    def save_command(self, job):
        with self.lock:
            self.db.execute(
                "INSERT OR REPLACE INTO commands(id,ts,op,target,label,state,cmd_id,replies,error,finished,auto) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                (job["id"], job["created"], job["op"], job["target"], job["label"], job["state"],
                 job.get("cmd_id"), json.dumps(job.get("replies") or {}), job.get("error", ""),
                 job.get("finished"), 1 if job.get("auto") else 0))
            self.db.commit()

    def recent_commands(self, node=None, limit=60, include_auto=False):
        where, args = [], []
        if node:
            where.append("target IN (?, 'all')")
            args.append(node)
        if not include_auto:
            where.append("auto = 0")
        w = (" WHERE " + " AND ".join(where)) if where else ""
        with self.lock:
            rows = self.db.execute(f"SELECT * FROM commands{w} ORDER BY ts DESC LIMIT ?",
                                   (*args, int(limit))).fetchall()
        out = []
        for r in rows:
            d = dict(r)
            d["replies"] = json.loads(d["replies"] or "{}")
            d["created"] = d.pop("ts")
            d["auto"] = bool(d["auto"])
            out.append(d)
        return out

    # ---------- GPS tracks (a node carried with a phone) ----------
    def add_track(self, node, lat, lon, acc, ts=None):
        with self.lock:
            self.db.execute("INSERT INTO tracks(ts,node,lat,lon,acc) VALUES(?,?,?,?,?)",
                            (ts or time.time(), node, lat, lon, acc))
            self.db.commit()

    def tracks(self, since):
        with self.lock:
            rows = self.db.execute("SELECT node, ts, lat, lon, acc FROM tracks WHERE ts >= ? ORDER BY ts",
                                   (since,)).fetchall()
        out = {}
        for r in rows:
            out.setdefault(r["node"], []).append([round(r["ts"], 1), r["lat"], r["lon"], r["acc"]])
        return out

    def clear_track(self, node):
        with self.lock:
            self.db.execute("DELETE FROM tracks WHERE node=?", (node,))
            self.db.commit()

    def clear_detections(self):
        with self.lock:
            self.db.execute("DELETE FROM detections")
            self.db.commit()

    # ---------- sightings ----------
    def add_sighting(self, node, event, rssi=None, pkts=None, ts=None):
        with self.lock:
            self.db.execute("INSERT INTO sightings(ts,node,event,rssi,pkts) VALUES(?,?,?,?,?)",
                            (ts or time.time(), node, event, rssi, pkts))
            self.db.commit()

    def node_sightings(self, node, since):
        with self.lock:
            rows = self.db.execute(
                "SELECT ts, event, rssi, pkts FROM sightings WHERE node=? AND ts>=? ORDER BY ts",
                (node, since)).fetchall()
        return [dict(r) for r in rows]

    def export_sightings(self, fmt):
        with self.lock:
            rows = [dict(r) for r in self.db.execute(
                "SELECT * FROM sightings ORDER BY ts ASC").fetchall()]
        if fmt == "json":
            return json.dumps(rows, indent=1)
        buf = io.StringIO()
        wr = csv.DictWriter(buf, fieldnames=["id", "ts", "node", "event", "rssi", "pkts"])
        wr.writeheader()
        wr.writerows(rows)
        return buf.getvalue()
