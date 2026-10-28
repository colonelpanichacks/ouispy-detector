"""Filter lists in the firmware's MESH_CMD_FILTERS wire format.

Record: u8 type, u8 flags (bit0 = full MAC), u8 idLen, u8 descLen, id, desc.
Validation mirrors filterRecordsValid() in src/main.cpp, and crc() equals
the firmware's crc32_le(0, ...) over the same bytes (standard CRC-32), so a
list's CRC here matches the `fcrc` a node reports in its RST status line.
"""
import os
import re
import zlib

# FilterType enum (src/main.cpp) <-> the codes the firmware prints.
TYPES = {"OUI": 0, "MAC": 1, "CID": 2, "SVC": 3, "NAME": 4, "META": 5, "PROBE": 6, "BEACON": 7}
CODES = {v: k for k, v in TYPES.items()}
HEX_LEN = {"OUI": 6, "PROBE": 6, "BEACON": 6, "MAC": 12, "CID": 4, "SVC": 4}
LABELS = {
    "OUI": "BLE OUI", "MAC": "BLE full MAC", "CID": "BLE company ID", "SVC": "BLE service UUID",
    "NAME": "BLE name contains", "META": "Meta composite", "PROBE": "WiFi probe OUI",
    "BEACON": "WiFi beacon OUI",
}

# Copies of the firmware presets (PRESET_* in src/main.cpp) for the editor.
PRESETS = {
    "axon": [("OUI", "0025DF", "Axon Enterprise OUI (IEEE)"),
             ("CID", "034D", "TASER International CID (Axon body cams)"),
             ("SVC", "FC81", "Axon Enterprise service UUID"),
             ("PROBE", "0025DF", "Axon body cam (WiFi probe)"),
             ("BEACON", "0025DF", "Axon AP beacon (WiFi)")],
    # Ray-Ban / Oakley Meta smart glasses: every Bluetooth SIG company ID and
    # 16-bit service UUID registered to Luxottica and Meta, the glasses'
    # BLE names, and Luxottica's IEEE OUIs. The composite is the precise
    # one; 01AB / FEB7 / FEB8 also match other Meta hardware (Quest, Portal).
    "meta": [
        ("META", "0x0D53+0xFD5F", "Luxottica CID + Meta svc, or glasses name"),
        ("CID", "0D53", "Luxottica CID (Ray-Ban / Oakley Meta glasses)"),
        ("SVC", "FD5F", "Meta Reality Labs svc UUID (glasses, Quest)"),
        ("CID", "058E", "Meta Platforms Technologies CID (glasses, Quest)"),
        ("CID", "01AB", "Meta Platforms CID (any Meta device)"),
        ("SVC", "FEB7", "Meta Platforms svc UUID (any Meta device)"),
        ("SVC", "FEB8", "Meta Platforms svc UUID (any Meta device)"),
        ("NAME", "Ray-Ban", "Ray-Ban Meta glasses (BLE name)"),
        ("NAME", "Oakley", "Oakley Meta glasses (BLE name)"),
        ("NAME", "Wayfarer", "Ray-Ban Meta Wayfarer (BLE name)"),
        ("OUI", "985949", "Luxottica Group OUI (IEEE)"),
        ("OUI", "80AA1C", "Luxottica Tristar OUI (IEEE, glasses plant)"),
        ("OUI", "384712", "Luxottica Tristar OUI (IEEE, glasses plant)"),
    ],
    "ipro": [("OUI", "D42DC5", "I-PRO body cam (BLE OUI)"),
             ("PROBE", "D42DC5", "I-PRO body cam (WiFi probe)")],
    "verkada": [("PROBE", "E0A700", "Verkada camera (WiFi probe)"),
                ("BEACON", "E0A700", "Verkada camera (WiFi beacon / setup AP)")],
}


class FilterError(ValueError):
    pass


def normalize(f):
    """{t, id, d} from the UI -> cleaned dict, or FilterError."""
    t = str(f.get("t", "")).upper()
    if t not in TYPES:
        raise FilterError(f"unknown type {t!r}")
    ident = str(f.get("id", "")).strip()
    if t in HEX_LEN:
        ident = ident.replace(":", "").replace("-", "").replace(" ", "").upper()
        if t in ("CID", "SVC") and ident.startswith("0X"):
            ident = ident[2:]
        if len(ident) != HEX_LEN[t] or any(c not in "0123456789ABCDEF" for c in ident):
            raise FilterError(f"{LABELS[t]} needs {HEX_LEN[t]} hex digits (got {f.get('id')!r})")
    else:
        if not 1 <= len(ident) <= 16 or any(not 0x20 <= ord(c) <= 0x7E for c in ident):
            raise FilterError(f"{LABELS[t]} needs 1-16 plain ASCII characters")
    desc = "".join(c for c in str(f.get("d", "")) if 0x20 <= ord(c) <= 0x7E)[:48]
    return {"t": t, "id": ident, "d": desc, "full": 1 if t == "MAC" else 0}


def encode(filters):
    """List of {t, id, d} -> wire bytes. Raises FilterError."""
    if not filters:
        raise FilterError("the list is empty: a node with no filters has nothing to scan for")
    if len(filters) > 100:
        raise FilterError("at most 100 filters")
    out = bytearray()
    seen = set()
    for f in filters:
        n = normalize(f)
        key = (n["t"], n["id"].upper())
        if key in seen:
            continue
        seen.add(key)
        ident, desc = n["id"].encode(), n["d"].encode()
        out += bytes([TYPES[n["t"]], n["full"], len(ident), len(desc)]) + ident + desc
    if len(out) > 4000:
        raise FilterError("list too large for one mesh transfer (4000 bytes)")
    return bytes(out)


def decode(buf):
    out, off = [], 0
    while off + 4 <= len(buf):
        t, flags, il, dl = buf[off:off + 4]
        off += 4
        out.append({"t": CODES.get(t, str(t)), "id": buf[off:off + il].decode("ascii", "replace"),
                    "d": buf[off + il:off + il + dl].decode("ascii", "replace"),
                    "full": flags & 1})
        off += il + dl
    return out


def crc(filters):
    """CRC of a list as a node would report it (fcrc), or None if invalid."""
    if not filters:
        return "00000000"        # crc32 of zero bytes, what an empty node reports
    try:
        return f"{zlib.crc32(encode(filters)) & 0xFFFFFFFF:08X}"
    except FilterError:
        return None


def chunks(buf, size=96):
    """Hex lines for the console's mfdata command (fits the USB RX buffer)."""
    return [buf[i:i + size].hex().upper() for i in range(0, len(buf), size)]


# ----------------------------------------------------------------------
# The portal's OUI database. Read live from BLEOUIs.md / WiFiOUIs.md in the
# repo (the same files sync-oui-list.py turns into the device portal), so
# the dashboard and the portal always offer the same vendors.
# ----------------------------------------------------------------------
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_RE_SUMMARY = re.compile(r"<summary><b>(.*?)</b>")
_RE_OUI = re.compile(r"^- `([0-9A-Fa-f:]{8})`")
_RE_META = re.compile(r"^> \*\*(Category|Detection Range|Common Devices|Frame Types):\*\*\s*(.*?)\s*$")


def _ascii(s):
    return "".join(c if 0x20 <= ord(c) <= 0x7E else "-" for c in s).replace("--", "-").strip()


def _parse_db(path, kind):
    try:
        lines = open(path, encoding="utf-8").read().splitlines()
    except OSError:
        return []
    out, cur = [], None
    for line in lines:
        line = line.strip()
        m = _RE_SUMMARY.search(line)
        if m:
            cur = {"label": _ascii(m.group(1)), "kind": kind, "ouis": [], "meta": {}}
            continue
        if cur is None:
            continue
        m = _RE_OUI.match(line)
        if m:
            o = m.group(1).replace(":", "").upper()
            if o not in cur["ouis"]:
                cur["ouis"].append(o)
            continue
        m = _RE_META.match(line)
        if m:
            cur["meta"][m.group(1)] = _ascii(m.group(2))
            continue
        if line.startswith("</details>"):
            if cur["ouis"]:
                cur["key"] = f"{kind}:{len(out)}"
                out.append(cur)
            cur = None
    return out


# Signatures the OUI lists can't express (they hold address prefixes only):
# device-name matches and the like. Installed as fixed filter rows.
EXTRAS = [
    {"label": "FLIPPER ZERO (BLE name + service UUIDs)",
     "rows": [("NAME", "Flipper", "Flipper Zero (BLE name)"),
              ("SVC", "3081", "Flipper Zero svc UUID (black)"),
              ("SVC", "3082", "Flipper Zero svc UUID (white)"),
              ("SVC", "3083", "Flipper Zero svc UUID (transparent)")],
     "meta": {"Category": "Pentest / Hacking Tool",
              "Common Devices": "Flipper Zero: BLE device name contains \"Flipper\"; advertises "
                                "16-bit service UUID 0x3081 / 0x3082 / 0x3083 by case color"}},
]


def vendor_db():
    """[{key, label, kind: ble|wifi|extra, ouis: [...], meta: {...}}]: the repo's
    OUI lists plus the extra (non-OUI) signatures."""
    extras = [{"key": f"extra:{i}", "label": e["label"], "kind": "extra", "ouis": [],
               "rows": [{"t": t, "id": i_, "d": d} for t, i_, d in e["rows"]], "meta": e["meta"]}
              for i, e in enumerate(EXTRAS)]
    return _parse_db(os.path.join(REPO, "BLEOUIs.md"), "ble") + \
           _parse_db(os.path.join(REPO, "WiFiOUIs.md"), "wifi") + extras


def vendor_filters(group, ble=True, wifi=True):
    """Filter rows for one database entry, exactly as the portal adds them:
    BLE entries -> BLE OUI and/or WiFi probe per OUI (both by default);
    WiFi entries -> probe and/or beacon per their Frame Types line."""
    if group["kind"] == "extra":
        return [dict(r) for r in group["rows"]]
    desc = group["label"][:32]
    rows = []
    for o in group["ouis"]:
        if group["kind"] == "ble":
            if ble:
                rows.append({"t": "OUI", "id": o, "d": desc})
            if wifi:
                rows.append({"t": "PROBE", "id": o, "d": desc})
        else:
            frames = group["meta"].get("Frame Types", "Probe Request").lower()
            if "probe" in frames:
                rows.append({"t": "PROBE", "id": o, "d": desc})
            if "beacon" in frames:
                rows.append({"t": "BEACON", "id": o, "d": desc})
    return rows


# ----------------------------------------------------------------------
# One signature library: the firmware presets and the OUI database used to
# be offered side by side, so AXON showed up as "AXON preset", "AXON (BLE)"
# and "AXON (WiFi)". Entries for the same vendor are merged here into one
# group holding every signature from every source, deduplicated by
# (type, identifier).
# ----------------------------------------------------------------------
_ALIASES = {"i pro": "ipro", "meta rayban smartglasses": "meta"}


def vendor_key(label):
    s = re.sub(r"\(.*?\)", " ", label.lower())
    s = re.sub(r"\bpreset\b", " ", s)
    s = re.sub(r"[^a-z0-9]+", " ", s).strip()
    return _ALIASES.get(s, s)


def library():
    """[{key, label, rows: [{t, id, d}], count, meta, sources}] sorted by label."""
    groups = {}

    def add(label, rows, meta=None):
        k = vendor_key(label)
        g = groups.setdefault(k, {"key": k, "label": re.sub(r"\s*\(.*?\)", "", label).strip().upper(),
                                  "rows": [], "meta": {}, "sources": []})
        have = {(r["t"], r["id"].upper()) for r in g["rows"]}
        for r in rows:
            if (r["t"], r["id"].upper()) not in have:
                have.add((r["t"], r["id"].upper()))
                g["rows"].append({"t": r["t"], "id": r["id"], "d": r["d"]})
        for mk, mv in (meta or {}).items():
            g["meta"].setdefault(mk, mv)
        g["sources"].append(label)

    for g in vendor_db():
        add(g["label"], vendor_filters(g), g["meta"])
    for name, rows in PRESETS.items():
        add(name, [{"t": t, "id": i, "d": d} for t, i, d in rows])
    out = sorted(groups.values(), key=lambda g: g["label"])
    for g in out:
        g["count"] = len(g["rows"])
    return out
