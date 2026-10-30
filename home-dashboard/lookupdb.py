"""The offline lookup database for the Tab5's microSD card: device intel with no dashboard.

    python lookupdb.py --out /Volumes/<card>/db/lookup.ldb      (the Tab5's card in a reader)

The same public registries the dashboard decodes with (lookups.py, data/): IEEE OUI
vendors, Bluetooth SIG company IDs, 16-bit service / member UUIDs and GAP appearances.

Format (sdcard.cpp reads it): 16-byte header {magic "OSLD1\\n\\0\\0", u32 count, u32 names
offset}, then count x {u8 table, 3 pad, u32 key, u32 name offset, u32 name length} sorted
by (table, key), then the names (UTF-8, no terminators). Tables:
  1 OUI (key = 24-bit prefix)   2 company ID   3 16-bit UUID
  4 appearance (key = category << 6 | subcategory; subcategory 0 = the category's name)
"""
import argparse
import os
import re
import struct
import sys

import lookups

T_OUI, T_CID, T_UUID, T_APPEAR = 1, 2, 3, 4


def entries():
    out = {}
    lookups.oui_vendor("00:00:00")   # loads the tables
    lookups.company("0x0000")
    lookups.uuid16("0x1800")
    lookups.appearance(0)
    for prefix, name in (lookups._oui or {}).items():
        out[(T_OUI, int(prefix, 16))] = name
    for tbl, src in ((T_CID, lookups._cid), (T_UUID, lookups._uuid), (T_APPEAR, lookups._appear)):
        for k, name in (src or {}).items():
            try:
                if isinstance(k, tuple):   # appearance: (category, subcategory or None)
                    cat, sub = k
                    sub = int(sub, 16) if isinstance(sub, str) else (sub or 0)
                    key = cat << 6 | sub
                else:
                    key = int(k, 16) if isinstance(k, str) else int(k)
            except (TypeError, ValueError):
                continue
            out[(tbl, key)] = name
    return out


def build(path):
    rows = sorted(entries().items())
    names, idx, off = bytearray(), bytearray(), 0
    for (tbl, key), name in rows:
        b = re.sub(r"\s+", " ", str(name)).strip().encode()[:80]
        idx += struct.pack("<B3xIII", tbl, key, len(names), len(b))
        names += b
    with open(path, "wb") as f:
        f.write(b"OSLD1\n\0\0" + struct.pack("<II", len(rows), 16 + len(idx)))
        f.write(idx)
        f.write(names)
    counts = {}
    for (tbl, _), _n in rows:
        counts[tbl] = counts.get(tbl, 0) + 1
    print(f"wrote {path}: {len(rows)} names ({counts.get(T_OUI, 0)} OUI, {counts.get(T_CID, 0)} companies, "
          f"{counts.get(T_UUID, 0)} UUIDs, {counts.get(T_APPEAR, 0)} appearances), {os.path.getsize(path) / 1e6:.1f} MB")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="the file to write (name it lookup.ldb)")
    a = ap.parse_args()
    build(a.out)


if __name__ == "__main__":
    sys.exit(main())
