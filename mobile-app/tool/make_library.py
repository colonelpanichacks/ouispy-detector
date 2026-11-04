#!/usr/bin/env python3
"""The signature library for the app's Detections area: the dashboard's own merged
library (home-dashboard/filters.py library(): the firmware presets plus BLEOUIs.md /
WiFiOUIs.md), so the phone offers exactly the vendors the dashboard and the portal do.

    python3 tool/make_library.py   # -> assets/library.json
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "home-dashboard"))
import filters  # noqa: E402

OUT = os.path.join(HERE, "..", "assets", "library.json")
lib = [{"key": g["key"], "label": g["label"], "rows": g["rows"], "meta": g.get("meta", {})} for g in filters.library()]
json.dump({"presets": sorted(filters.PRESETS), "library": lib}, open(OUT, "w"), separators=(",", ":"))
print(OUT, len(lib), "vendors", sum(len(g["rows"]) for g in lib), "signatures")
