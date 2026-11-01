"""Map marks: pins (an icon, a colour, a name) and named areas (a polygon), drawn on the
dashboard map and the Tab5's. Either side makes them; the id comes from whoever made it,
so a Tab5 can make one offline and send it later without making a second copy.

A mark: {id, kind: "pin" | "area", name, hue (0-359), icon, lat, lon, pts: [[lat, lon], ...],
notes, ts}. Every change goes to every client as the "marks" event (the whole list: it stays
small)."""

import json
import re
import time

ICONS = ["pin", "flag", "target", "house", "car", "person", "drone", "camera", "warning", "star",
         "camp", "radio"]
MAX_MARKS = 200
MAX_PTS = 24

SCHEMA = """
CREATE TABLE IF NOT EXISTS map_marks (
    id    TEXT PRIMARY KEY,
    kind  TEXT NOT NULL,
    name  TEXT,
    hue   INTEGER,
    icon  TEXT,
    lat   REAL, lon REAL,
    pts   TEXT,
    notes TEXT,
    ts    REAL
);
"""


def _num(v, lo, hi):
    try:
        v = float(v)
    except (TypeError, ValueError):
        return None
    return v if lo <= v <= hi else None


class Marks:
    def __init__(self, store, emit):
        self.store, self.emit = store, emit
        with store.lock:
            store.db.executescript(SCHEMA)
            store.db.commit()

    def all(self):
        st = self.store
        with st.lock:
            rows = st.db.execute("SELECT * FROM map_marks ORDER BY ts").fetchall()
        out = []
        for r in rows:
            m = dict(r)
            try:
                m["pts"] = json.loads(m["pts"]) if m["pts"] else []
            except ValueError:
                m["pts"] = []
            out.append(m)
        return out

    def save(self, data):
        """Create or update one mark; returns it, or raises ValueError with why not."""
        mid = str(data.get("id") or "").strip()
        if not re.fullmatch(r"[0-9A-Za-z_-]{4,24}", mid):
            mid = "%x" % int(time.time() * 1000)
        kind = data.get("kind") if data.get("kind") in ("pin", "area") else None
        cur = next((m for m in self.all() if m["id"] == mid), None)
        if not kind:
            if not cur:
                raise ValueError("kind must be pin or area")
            kind = cur["kind"]
        m = dict(cur or {"id": mid, "kind": kind, "name": "", "hue": 190, "icon": "pin",
                         "lat": None, "lon": None, "pts": [], "notes": ""})
        if "name" in data:
            m["name"] = str(data.get("name") or "").strip()[:40]
        if "notes" in data:
            m["notes"] = str(data.get("notes") or "")[:400]
        if "hue" in data and _num(data["hue"], 0, 360) is not None:
            m["hue"] = int(_num(data["hue"], 0, 360)) % 360
        if data.get("icon") in ICONS:
            m["icon"] = data["icon"]
        if kind == "pin":
            la, lo = _num(data.get("lat"), -90, 90), _num(data.get("lon"), -180, 180)
            if la is not None and lo is not None:
                m["lat"], m["lon"] = la, lo
            if m["lat"] is None:
                raise ValueError("a pin needs lat and lon")
        else:
            if "pts" in data:
                pts = []
                for p in (data.get("pts") or [])[:MAX_PTS]:
                    la, lo = (_num(p[0], -90, 90), _num(p[1], -180, 180)) if isinstance(p, (list, tuple)) and len(p) == 2 else (None, None)
                    if la is not None and lo is not None:
                        pts.append([la, lo])
                m["pts"] = pts
            if len(m["pts"]) < 3:
                raise ValueError("an area needs 3 or more points")
            m["lat"] = sum(p[0] for p in m["pts"]) / len(m["pts"])
            m["lon"] = sum(p[1] for p in m["pts"]) / len(m["pts"])
        m["ts"] = time.time()
        st = self.store
        with st.lock:
            if not cur and st.db.execute("SELECT COUNT(*) FROM map_marks").fetchone()[0] >= MAX_MARKS:
                raise ValueError("too many marks (%d)" % MAX_MARKS)
            st.db.execute("INSERT OR REPLACE INTO map_marks(id, kind, name, hue, icon, lat, lon, pts, notes, ts) "
                          "VALUES(?,?,?,?,?,?,?,?,?,?)",
                          (m["id"], kind, m["name"], m["hue"], m["icon"], m["lat"], m["lon"],
                           json.dumps(m["pts"]), m["notes"], m["ts"]))
            st.db.commit()
        self.broadcast()
        return m

    def delete(self, mid):
        st = self.store
        with st.lock:
            n = st.db.execute("DELETE FROM map_marks WHERE id=?", (mid,)).rowcount
            st.db.commit()
        if n:
            self.broadcast()
        return bool(n)

    def broadcast(self):
        self.emit("marks", {"marks": self.all()})
