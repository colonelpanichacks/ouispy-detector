#!/usr/bin/env python3
# sync the OUI list from ouis.md into the OUI-SPY firmware config page.

import re
import sys
import os

# Constants:

OUI_LIST_SOURCE = "./ouis.md"
MAIN_CPP = "src/main.cpp"

START_MARKER = "<!-- OUI_DB_START -->"
END_MARKER = "<!-- OUI_DB_END -->"

# Indentation inside the raw HTML string in main.cpp
INDENT = "                    "


def convert_ouis_md_to_html():
    with open(OUI_LIST_SOURCE, "r", encoding="utf-8") as f:
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
    oui_entries = []  # Collect OUI list items for inline <code> display

    def flush_oui_entries():
        nonlocal oui_entries
        if oui_entries:
            codes = " ".join(f"<code>{oui}</code>" for oui in oui_entries)
            output.append(f"<div class=\"oui-entries\">{codes}</div>")
            oui_entries = []

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
                # Flush any collected OUI entries before the button
                flush_oui_entries()
                # Use \\n so it appears as literal \n in the C++ raw string,
                # which JavaScript interprets as a newline character
                oui_data = ",".join(code_lines)
                output.append(
                    '<button type="button" class="oui-add-btn" '
                    "onclick=\"appendOUIs('" + oui_data + "')\">+ Add to filter list</button>"
                )
                i += 1
                continue

        if in_code_block:
            if stripped:
                code_lines.append(stripped)
            i += 1
            continue

        # Skip "**Copy OUIs:**" label 
        if stripped == "**Copy OUIs:**":
            i += 1
            continue

        # Skip empty lines
        if stripped == "":
            i += 1
            continue

        if stripped in ("<details>", "</details>"):
            flush_oui_entries()
            output.append(stripped)
            i += 1
            continue

        if stripped.startswith("<summary>"):
            flush_oui_entries()
            output.append(stripped)
            i += 1
            continue

        list_match = re.match(r"^-\s*`([^`]+)`$", stripped)
        if list_match:
            oui_entries.append(list_match.group(1))
            i += 1
            continue

        # Flush OUI entries before any other content
        flush_oui_entries()

        bq_match = re.match(r"^>\s*(.+)$", stripped)
        if bq_match:
            inner = bq_match.group(1).strip()
            # Strip trailing whitespace markers from markdown
            inner = inner.rstrip()
            inner = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", inner)
            output.append(f"<div class=\"oui-meta\">{inner}</div>")
            i += 1
            continue

        processed = stripped
        # Bold
        processed = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", processed)
        # Italic (single *)
        processed = re.sub(r"(?<!\*)\*([^*]+)\*(?!\*)", r"<em>\1</em>", processed)
        # Links
        processed = re.sub(
            r"\[([^\]]+)\]\(([^)]+)\)",
            r"<a href=\"\2\" target=\"_blank\" style=\"color:#4ecdc4;\">\1</a>",
            processed
        )
        output.append(f"<div class=\"oui-note\">{processed}</div>")
        i += 1

    flush_oui_entries()

    # Join with newlines and proper indentation for the C++ raw string
    return ("\n" + INDENT).join(output)


def inject_into_main_cpp(html_content):
    """Replace content between OUI_DB markers in main.cpp with generated HTML."""
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

    print(f"Successfully injected OUI database into {MAIN_CPP}")


if __name__ == "__main__":
    print(f"Reading OUI database from: {OUI_LIST_SOURCE}")
    html = convert_ouis_md_to_html()

    print(f"Generated {len(html)} bytes of HTML")
    print(f"Injecting into: {MAIN_CPP}")
    inject_into_main_cpp(html)
