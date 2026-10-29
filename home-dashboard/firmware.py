"""Firmware images for updates over the mesh.

The dashboard keeps one uploaded image (firmware/staged.bin, git-ignored)
plus what it read from it. Only an ESP32-S3 application image is accepted:
the ESP-IDF image header (magic 0xE9, chip id 9) followed by the app
description (magic 0xABCD5432), which carries the project name, version and
build date the firmware itself was compiled with.
"""
import hashlib
import json
import os
import re
import struct
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
DIR = os.path.join(HERE, "firmware")
BIN = os.path.join(DIR, "staged.bin")
META = os.path.join(DIR, "staged.json")
MAX_SIZE = 0x330000          # one app slot of default_8MB.csv
CHIP_ESP32S3 = 9
APP_DESC_MAGIC = 0xABCD5432
CHUNK = 512                  # bytes per fwdata console line


class FirmwareError(ValueError):
    pass


def _cstr(b):
    return b.split(b"\0", 1)[0].decode("ascii", "replace").strip()


_STAMP = re.compile(rb"([A-Z][a-z]{2} [ \d]\d \d{4}) (\d\d:\d\d:\d\d)")


def build_stamp(img):
    """The newest __DATE__ __TIME__ string in the image: the OUI-SPY firmware
    embeds its build time (FW_BUILD, what nodes report as "fw"); the app
    description only carries the Arduino core's own build date."""
    best = None
    for m in _STAMP.finditer(img):
        try:
            when = datetime.strptime(m.group(1).decode().replace("  ", " ") + " " + m.group(2).decode(), "%b %d %Y %H:%M:%S")
        except ValueError:
            continue
        if not best or when > best[0]:
            best = (when, m.group(1).decode() + " " + m.group(2).decode())
    return best[1] if best else ""


def inspect(img):
    """Validate an image -> {size, sha256, project, version, date, time, idf, build}."""
    if not 64 * 1024 <= len(img) <= MAX_SIZE:
        raise FirmwareError(f"size {len(img)} bytes is not a plausible app image (64 KB to {MAX_SIZE // 1024} KB)")
    if img[0] != 0xE9:
        raise FirmwareError("not an ESP32 application image (use firmware.bin, not a merged or bootloader file)")
    chip = struct.unpack_from("<H", img, 12)[0]
    if chip != CHIP_ESP32S3:
        raise FirmwareError(f"built for chip id {chip}, not the ESP32-S3 these nodes use")
    if struct.unpack_from("<I", img, 32)[0] != APP_DESC_MAGIC:
        raise FirmwareError("no ESP-IDF app description in the image")
    return {
        "size": len(img),
        "sha256": hashlib.sha256(img).hexdigest(),
        "version": _cstr(img[48:80]),
        "project": _cstr(img[80:112]),
        "time": _cstr(img[112:128]),
        "date": _cstr(img[128:144]),
        "idf": _cstr(img[144:176]),
        "build": build_stamp(img),
        # Which build of the node firmware: "detector" (OUI-SPY Mesh node) or
        # "skyspy" (dedicated drone node). Images from before the marker are detectors.
        "kind": _kind(img),
    }


def _kind(img):
    i = img.find(b"OUISPY-KIND=")
    if i < 0:
        return "detector"
    j = i + 12
    k = j
    while k < len(img) and k - j < 16 and 0x61 <= img[k] <= 0x7a:
        k += 1
    return img[j:k].decode() or "detector"


def node_kind(fw):
    """A node's kind from the build stamp it reports ("Oct28 20:37:51S" = Sky-Spy,
    "...W" = wardriver)."""
    end = (fw or "").rstrip()[-1:]
    return {"S": "skyspy", "W": "wardrive"}.get(end, "detector")


def save(img, filename=""):
    meta = inspect(img)
    meta["filename"] = os.path.basename(filename or "")[:80]
    os.makedirs(DIR, exist_ok=True)
    with open(BIN, "wb") as f:
        f.write(img)
    with open(META, "w") as f:
        json.dump(meta, f)
    return meta


def current():
    """The uploaded image's info, or None."""
    try:
        with open(META) as f:
            meta = json.load(f)
        if os.path.getsize(BIN) != meta["size"]:
            return None
        return meta
    except (OSError, ValueError, KeyError):
        return None


def load():
    meta = current()
    if not meta:
        raise FirmwareError("no firmware uploaded")
    with open(BIN, "rb") as f:
        img = f.read()
    if hashlib.sha256(img).hexdigest() != meta["sha256"]:
        raise FirmwareError("the uploaded image changed on disk; upload it again")
    return img, meta
