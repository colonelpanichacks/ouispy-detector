"""Offline map packs for the Tab5's microSD card (and anything else that reads them).

    python tilepack.py --out /Volumes/TAB5SD/maps --name buncombe \
        --bbox 35.42,-82.89,35.83,-82.25 --zmin 0 --zmax 19
    python tilepack.py --out ... --name nc --bbox 33.75,-84.33,36.6,-75.4 --zmax 13
    Write straight onto the Tab5's card in a card reader: --out /Volumes/<card>/maps

Imagery, both public and free to keep offline:
  z0-16   USGS The National Map, USGSImageryOnly tiles (public domain)
  z17-19  NC OneMap statewide orthoimagery (6 inch), exported as 2048 px images
          (64 tiles per request, polite to the state's server) and cut into tiles here
Outside North Carolina the deep zooms come from USGS too (it stops at 16; the Tab5
over-zooms past that).

Pack format (sdcard.h on the Tab5 reads it): 64-byte header {magic "OSPK1\\n\\0\\0", layer
char[16], u32 count, u64 first key, u64 last key, zeros}, then count x {u64 key, u32 offset,
u32 length} sorted by key (key = z << 48 | x << 24 | y), then the JPEG tiles. Each pack stays
under 1.9 GB (FAT32, and the Tab5 seeks with a signed 32-bit offset), so a big area is
several files: <name>-z<from>-<to>.pak for the shallow zooms, <name>-z<z>[-<strip>].pak
for each deep one.

Resumable: each pack's tiles go into its own SQLite work file next to it first, and a
re-run skips finished packs and only fetches what a half-done one is missing. The work file
goes once its pack is written (--keep to keep it), so the card only ever holds one pack's
scratch copy on top of the finished packs.
"""
import argparse
import io
import math
import os
import sqlite3
import struct
import sys
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor

from PIL import Image

LAYER = "esri-sat"          # the Tab5's satellite layer id: the packs stand in for it
USGS = "https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryOnly/MapServer/tile/{z}/{y}/{x}"
NCONEMAP = ("https://services.nconemap.gov/secure/rest/services/Imagery/Orthoimagery_Latest/ImageServer/exportImage"
            "?bbox={x0},{y0},{x1},{y1}&bboxSR=3857&imageSR=3857&size={px},{px}&format=jpg"
            "&compressionQuality=85&f=image")
NC_BOX = (33.75, -84.33, 36.6, -75.4)   # deep imagery only where NC OneMap has it
USGS_MAX = 16
BLOCK = 8                   # NC OneMap request = BLOCK x BLOCK tiles
PACK_MAX = 1_900_000_000
UA = {"User-Agent": "OUI-SPY Mesh tilepack (offline field map)"}
R = 6378137.0
HALF = math.pi * R


def tile_xy(lat, lon, z):
    n = 2 ** z
    x = int((lon + 180) / 360 * n)
    r = math.radians(lat)
    y = int((1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2 * n)
    return min(max(x, 0), n - 1), min(max(y, 0), n - 1)


def tile_merc(z, x, y):
    """(x0, y0, x1, y1) of a tile in Web Mercator metres."""
    size = 2 * HALF / 2 ** z
    return -HALF + x * size, HALF - (y + 1) * size, -HALF + (x + 1) * size, HALF - y * size


def tile_lat(z, y):
    n = math.pi - 2 * math.pi * y / 2 ** z
    return math.degrees(math.atan(math.sinh(n)))


def tile_lon(z, x):
    return x / 2 ** z * 360 - 180


def in_nc(z, x, y):
    s, w, n, e = NC_BOX
    return tile_lat(z, y + 1) < n and tile_lat(z, y) > s and tile_lon(z, x + 1) > w and tile_lon(z, x) < e


def key(z, x, y):
    return z << 48 | x << 24 | y


def get(url, tries=5):
    for i in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
                return r.read()
        except Exception as e:
            if getattr(e, "code", None) == 404:
                return None
            time.sleep(2 * (i + 1))
    return None


def blank(jpg):
    """USGS answers outside its coverage with a flat tile: not worth keeping."""
    try:
        im = Image.open(io.BytesIO(jpg)).convert("L")
        lo, hi = im.getextrema()
        return hi - lo < 4
    except Exception:
        return True


class Work:
    def __init__(self, path):
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.execute("CREATE TABLE IF NOT EXISTS t (k INTEGER PRIMARY KEY, img BLOB)")
        self.db.execute("CREATE TABLE IF NOT EXISTS done_block (k INTEGER PRIMARY KEY)")
        self.lock = threading.Lock()

    def have(self, k, table="t"):
        with self.lock:
            return self.db.execute(f"SELECT 1 FROM {table} WHERE k=?", (k,)).fetchone() is not None

    def put(self, rows, block=None):
        with self.lock:
            self.db.executemany("INSERT OR REPLACE INTO t VALUES (?,?)", rows)
            if block is not None:
                self.db.execute("INSERT OR IGNORE INTO done_block VALUES (?)", (block,))
            self.db.commit()


def usgs_job(work, z, x, y):
    k = key(z, x, y)
    if work.have(k):
        return 0
    img = get(USGS.format(z=z, x=x, y=y))
    if img and not blank(img):
        work.put([(k, img)])
        return 1
    return 0


def nc_job(work, z, bx, by, xr, yr):
    """One NC OneMap export of BLOCK x BLOCK tiles, cut up; tiles outside the area skipped."""
    bk = key(z, bx, by)
    if work.have(bk, "done_block"):
        return 0
    x0, y0, _, _ = tile_merc(z, bx, by + BLOCK - 1)
    _, _, x1, y1 = tile_merc(z, bx + BLOCK - 1, by)
    img = get(NCONEMAP.format(x0=x0, y0=y0, x1=x1, y1=y1, px=256 * BLOCK))
    if not img:
        return 0
    im = Image.open(io.BytesIO(img)).convert("RGB")
    rows = []
    for dy in range(BLOCK):
        for dx in range(BLOCK):
            x, y = bx + dx, by + dy
            if not (xr[0] <= x <= xr[1] and yr[0] <= y <= yr[1]):
                continue
            t = im.crop((dx * 256, dy * 256, dx * 256 + 256, dy * 256 + 256))
            lo, hi = t.convert("L").getextrema()
            if hi - lo < 4:
                continue   # outside the imagery (flat fill)
            b = io.BytesIO()
            t.save(b, "JPEG", quality=80, optimize=True)
            rows.append((key(z, x, y), b.getvalue()))
    work.put(rows, block=bk)
    return len(rows)


def write_pack(work, path):
    """Every tile in the work file into one pack."""
    rows = work.db.execute("SELECT k, length(img) FROM t ORDER BY k").fetchall()
    if not rows:
        return 0, 0
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        hdr = b"OSPK1\n\0\0" + LAYER.encode().ljust(16, b"\0") + struct.pack("<IQQ", len(rows), rows[0][0], rows[-1][0])
        f.write(hdr.ljust(64, b"\0"))
        off = 64 + 16 * len(rows)
        for k, ln in rows:
            f.write(struct.pack("<QII", k, off, ln))
            off += ln
        for (img,) in work.db.execute("SELECT img FROM t ORDER BY k"):
            f.write(img)
    os.replace(tmp, path)
    return len(rows), os.path.getsize(path)


def fetch(work, z, xr, yr, threads):
    t0, got = time.time(), 0
    with ThreadPoolExecutor(threads) as ex:
        if z <= USGS_MAX:
            futs = [ex.submit(usgs_job, work, z, x, y) for x in range(xr[0], xr[1] + 1) for y in range(yr[0], yr[1] + 1)]
        else:
            futs = [ex.submit(nc_job, work, z, bx, by, xr, yr)
                    for bx in range(xr[0] - xr[0] % BLOCK, xr[1] + 1, BLOCK)
                    for by in range(yr[0] - yr[0] % BLOCK, yr[1] + 1, BLOCK)
                    if in_nc(z, bx + BLOCK // 2, by + BLOCK // 2)]   # outside NC: the Tab5 over-zooms z16
        for i, f in enumerate(futs):
            got += f.result() or 0
            if i % 200 == 0:
                print(f"z{z} x{xr[0]}-{xr[1]}: {i + 1}/{len(futs)} jobs, {got} new tiles, {time.time() - t0:.0f} s", flush=True)
    return got


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="folder for the packs (the card's /maps)")
    ap.add_argument("--name", required=True)
    ap.add_argument("--bbox", required=True, help="south,west,north,east")
    ap.add_argument("--zmin", type=int, default=0)
    ap.add_argument("--zmax", type=int, default=19)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--keep", action="store_true", help="keep the work file")
    ap.add_argument("--work", help="where the work file goes (default: --out)")
    a = ap.parse_args()
    s, w, n, e = (float(v) for v in a.bbox.split(","))
    os.makedirs(a.out, exist_ok=True)
    wdir = a.work or a.out
    # Pack groups, each fetched into its own work file, written out, then the work file
    # deleted: the card never holds more than one group's scratch copy. Shallow zooms
    # share one pack; a deep zoom is cut into column strips of about PACK_MAX each.
    groups = []
    shallow = [z for z in range(a.zmin, a.zmax + 1) if z <= USGS_MAX]
    if shallow:
        groups.append((f"{a.name}-z{shallow[0]}-{shallow[-1]}", [(z, None) for z in shallow]))
    for z in range(max(a.zmin, USGS_MAX + 1), a.zmax + 1):
        (x0, y0), (x1, y1) = tile_xy(n, w, z), tile_xy(s, e, z)
        est = (x1 - x0 + 1) * (y1 - y0 + 1) * 20_000
        strips = max(1, math.ceil(est / (PACK_MAX * 0.85)))
        ax = x0 - x0 % BLOCK   # strips start on export-block edges
        width = math.ceil((x1 - ax + 1) / strips / BLOCK) * BLOCK
        cuts = list(range(ax, x1 + 1, width))
        for i, c in enumerate(cuts):
            xr = (max(c, x0), min(c + width - 1, x1))
            groups.append((f"{a.name}-z{z}" + (f"-{i + 1}" if len(cuts) > 1 else ""), [(z, xr)]))
    for label, parts in groups:
        path = os.path.join(a.out, label + ".pak")
        if os.path.exists(path):
            print(f"{label}.pak already there, skipped", flush=True)
            continue
        wpath = os.path.join(wdir, f"{label}.work.sqlite")
        work = Work(wpath)
        for z, xr in parts:
            (x0, y0), (x1, y1) = tile_xy(n, w, z), tile_xy(s, e, z)
            fetch(work, z, xr or (x0, x1), (y0, y1), a.threads)
        cnt, sz = write_pack(work, path)
        work.db.close()
        if not a.keep:
            os.remove(wpath)
        if not cnt and os.path.exists(path):
            os.remove(path)
        print(f"wrote {label}.pak: {cnt} tiles, {sz / 1e6:.0f} MB", flush=True)
    print("done", flush=True)


if __name__ == "__main__":
    sys.exit(main())
