"""Filter lists in the firmware's MESH_CMD_FILTERS wire format.

Record: u8 type, u8 flags (bit0 = full MAC), u8 idLen, u8 descLen, id, desc.
Validation mirrors filterRecordsValid() in src/main.cpp, and crc() equals
the firmware's crc32_le(0, ...) over the same bytes (standard CRC-32), so a
list's CRC here matches the `fcrc` a node reports in its RST status line.
"""
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
    "meta": [("META", "0x0D53+0xFD5F",
              "Composite: Luxottica CID + Meta svc UUID, or Ray-Ban/Wayfarer/Oakley Meta name")],
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
