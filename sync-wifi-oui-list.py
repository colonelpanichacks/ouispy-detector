#!/usr/bin/env python3
# sync the WiFi OUI list from WiFiOUIs.md into the OUI-SPY firmware config
# page's "WiFi Signatures" sub-section.
#
# Unlike the BLE OUI database (which appends MAC prefixes to the OUI
# textarea via appendOUIs()), WiFi OUIs install FT_WIFI_PROBE /
# FT_WIFI_BEACON filters through the preset endpoints, so each vendor card
# emits an addVendor('<preset_key>', '<label>', '<oui>') button instead.

import re
import sys

# Constants:

WIFI_OUI_LIST_SOURCE = "./WiFiOUIs.md"
MAIN_CPP = "src/main.cpp"

START_MARKER = "<!-- WIFI_OUI_DB_START -->"
END_MARKER = "<!-- WIFI_OUI_DB_END -->"

# Indentation inside the raw HTML string in main.cpp
INDENT = "                    "

# Map a vendor's <summary> display name to the firmware preset key the
# /api/presets/* endpoints understand. Vendors not listed fall back to a
# slugified version of the name (lowercase, non-alnum -> underscore).
VENDOR_PRESET_KEYS = {
    "AXON (WiFi)": "axon_wifi",
    "AXON": "axon_wifi",
    "I-PRO": "ipro",
}


def slugify(name):
    s = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
    return s


def convert_wifi_ouis_md_to_html():
    with open(WIFI_OUI_LIST_SOURCE, "r", encoding="utf-8") as f:
        content = f.read()

    match = re.search(
        r'## Categorized by Manufacturer\s*\n(.*?)(?=\n---\s*\n)',
        content,
        re.DOTALL
    )
    if not match:
        sys.exit(1)

    section = match.group(1).strip()
    lines = section.split("\n")

    output = []
    i = 0
    in_code_block = False
    code_lines = []
    # Per-card buffers. Everything is accumulated while a card is open and
    # emitted in the correct order (details/summary/entries/button/meta/note)
    # only when the card closes, so the generated HTML is well-formed.
    card = None   # dict when a <details> card is open, else None

    def flush_card():
        nonlocal card
        if card is None:
            return
        vendor = card["vendor"]
        entries = card["entries"]
        rep_oui = card["oui"] if card["oui"] else (entries[0] if entries else "")
        preset_key = VENDOR_PRESET_KEYS.get(vendor, slugify(vendor))
        codes = " ".join(f"<code>{o}</code>" for o in entries) if entries else ""
        output.append("<details>")
        output.append(
            f'<summary><b>{vendor}</b> '
            f'<code>{len(entries)} OUI{"s" if len(entries) != 1 else ""}</code></summary>'
        )
        if codes:
            output.append(f'<div class="oui-entries">{codes}</div>')
        output.append(
            '<button type="button" class="oui-add-btn" '
            f"onclick=\"addVendor('{preset_key}','{vendor}','{rep_oui}')\">+ Add WiFi signatures</button>"
        )
        output.extend(card["meta"])
        output.extend(card["note"])
        output.append("</details>")
        card = None

    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        if stripped == "```":
            if not in_code_block:
                in_code_block = True
                code_lines = []
                i += 1
                continue
            else:
                in_code_block = False
                if code_lines and card is not None:
                    card["oui"] = code_lines[0]
                i += 1
                continue

        if in_code_block:
            if stripped:
                code_lines.append(stripped)
            i += 1
            continue

        if stripped == "**Copy OUIs:**":
            i += 1
            continue

        if stripped == "":
            i += 1
            continue

        if stripped == "<details>":
            flush_card()
            card = {"vendor": None, "entries": [], "oui": None, "meta": [], "note": []}
            i += 1
            continue

        if stripped == "</details>":
            flush_card()
            i += 1
            continue

        if stripped.startswith("<summary>"):
            # e.g. <summary><b>AXON</b> <code>1 OUI</code></summary>
            m = re.search(r"<b>([^<]+)</b>", stripped)
            if m and card is not None:
                card["vendor"] = m.group(1).strip()
            i += 1
            continue

        list_match = re.match(r"^-\s*`([^`]+)`$", stripped)
        if list_match:
            if card is not None:
                card["entries"].append(list_match.group(1))
            i += 1
            continue

        bq_match = re.match(r"^>\s*(.+)$", stripped)
        if bq_match:
            if card is not None:
                inner = bq_match.group(1).strip().rstrip()
                inner = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", inner)
                card["meta"].append(f"<div class=\"oui-meta\">{inner}</div>")
            i += 1
            continue

        if card is not None:
            processed = stripped
            processed = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", processed)
            processed = re.sub(r"(?<!\*)\*([^*]+)\*(?!\*)", r"<em>\1</em>", processed)
            processed = re.sub(
                r"\[([^\]]+)\]\(([^)]+)\)",
                r"<a href=\"\2\" target=\"_blank\" style=\"color:#4ecdc4;\">\1</a>",
                processed
            )
            card["note"].append(f"<div class=\"oui-note\">{processed}</div>")
        i += 1

    flush_card()

    # Join with newlines and proper indentation for the C++ raw string
    return ("\n" + INDENT).join(output)


def inject_into_main_cpp(html_content):
    """Replace content between WIFI_OUI_DB markers in main.cpp with generated HTML."""
    with open(MAIN_CPP, "r", encoding="utf-8") as f:
        cpp = f.read()

    start_idx = cpp.find(START_MARKER)
    end_idx = cpp.find(END_MARKER)

    if start_idx == -1 or end_idx == -1:
        print(f"ERROR: Markers not found in {MAIN_CPP}")
        print(f"  Looking for: {START_MARKER} ... {END_MARKER}")
        print(f"  Make sure the config HTML contains these markers.")
        sys.exit(1)

    end_idx += len(END_MARKER)
    replacement = f"{START_MARKER}\n{INDENT}{html_content}\n{INDENT}{END_MARKER}"
    new_cpp = cpp[:start_idx] + replacement + cpp[end_idx:]

    with open(MAIN_CPP, "w", encoding="utf-8") as f:
        f.write(new_cpp)

    print(f"Successfully injected WiFi OUI database into {MAIN_CPP}")


if __name__ == "__main__":
    print(f"Reading WiFi OUI database from: {WIFI_OUI_LIST_SOURCE}")
    html = convert_wifi_ouis_md_to_html()

    print(f"Generated {len(html)} bytes of HTML")
    print(f"Injecting into: {MAIN_CPP}")
    inject_into_main_cpp(html)
