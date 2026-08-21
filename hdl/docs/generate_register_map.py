#!/usr/bin/env python3
"""generate_register_map.py — renders config/hdl_register_map.json into
hdl/docs/axi4lite_register_map.md.

config/hdl_register_map.json is the source of truth (Phase 9.0 deliverable,
see the HDL port plan); this script only formats it. Re-run after any edit
to the JSON so the two never drift:

    python3 hdl/docs/generate_register_map.py
"""

import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
JSON_PATH = ROOT / "config" / "hdl_register_map.json"
MD_PATH = ROOT / "hdl" / "docs" / "axi4lite_register_map.md"


def fmt_reset(reg):
    reset = reg.get("reset")
    if reset is None:
        return "*(TBD)*"
    if reg.get("format") == "q1_15":
        return f"`{reset}` ({reg.get('reset_float')})"
    if reg.get("format") == "milli_fixed32":
        return f"`{reset}` ({reg.get('reset_float')})"
    if isinstance(reset, bool):
        return "`1`" if reset else "`0`"
    if isinstance(reset, str):
        return f"`{reset}`"
    return f"`{reset}`"


def render_register_table(registers, out):
    out.append("| Register | Offset | Format | Reset | Writable by | Notes |")
    out.append("|---|---|---|---|---|---|")
    for reg in registers:
        name = reg["name"]
        offset = reg["offset"]
        fmt = reg["format"]
        reset = fmt_reset(reg)
        writable = reg["writable_by"]
        notes = reg.get("notes", "")
        c_source = reg.get("c_source", "")
        note_cell = notes
        if c_source:
            note_cell = f"{notes} *(`{c_source}`)*" if notes else f"*(`{c_source}`)*"
        out.append(f"| `{name}` | `{offset}` | {fmt} | {reset} | {writable} | {note_cell} |")
    out.append("")


def render_excluded(excluded, out):
    if not excluded:
        return
    out.append("**Excluded fields** (present in the C++ config struct, deliberately not a register):")
    out.append("")
    for e in excluded:
        out.append(f"- `{e['field']}` — {e['reason']}")
    out.append("")


def main():
    data = json.loads(JSON_PATH.read_text())

    out = []
    out.append(f"# {data['title']}")
    out.append("")
    out.append(
        f"> Generated from `config/hdl_register_map.json` (v{data['version']}) by "
        f"`hdl/docs/generate_register_map.py`. Edit the JSON, not this file, and "
        f"re-run the script."
    )
    out.append("")
    out.append(data["description"])
    out.append("")

    out.append("## Conventions")
    out.append("")
    conv = data["conventions"]
    out.append(f"**Address layout:** {conv['address_layout']}")
    out.append("")
    out.append("**Write ownership (`writable_by`):**")
    out.append("")
    for k, v in conv["writable_by"].items():
        out.append(f"- `{k}` — {v}")
    out.append("")
    out.append("**Register formats:**")
    out.append("")
    for k, v in conv["formats"].items():
        out.append(f"- `{k}` — {v}")
    out.append("")
    out.append(f"**Pilot pattern source:** {conv['pilot_pattern_source']}")
    out.append("")

    out.append("## Known default inconsistencies in the golden model")
    out.append("")
    out.append(
        "Found while cross-referencing every block's C++ config defaults against "
        "each other to build this register map. Not fixed here -- flagged as "
        "candidate `lib/` fixes for a future bugfix branch."
    )
    out.append("")
    for item in data["known_default_inconsistencies"]:
        out.append(f"### `{item['field']}`")
        out.append("")
        out.append(item["description"])
        out.append("")
        out.append(f"**Resolution used in this register map:** {item['resolution']}")
        out.append("")

    l1 = data["l1_status"]
    out.append("## `l1_status` — canonical L1-decode mirror")
    out.append("")
    out.append(f"Base address: `{l1['base_address']}`")
    out.append("")
    out.append(l1["description"])
    out.append("")
    render_register_table(l1["registers"], out)
    render_excluded(l1.get("excluded_fields", []), out)

    out.append("## Per-block registers")
    out.append("")
    for block_name, block in data["blocks"].items():
        out.append(f"### `{block_name}` (Phase {block['phase']})")
        out.append("")
        out.append(f"Base address: `{block['base_address']}`")
        out.append("")
        if "description" in block:
            out.append(block["description"])
            out.append("")
        render_register_table(block["registers"], out)
        render_excluded(block.get("excluded_fields", []), out)
        for item in block.get("open_items", []):
            out.append(f"**Open item:** {item}")
            out.append("")

    oos = data.get("explicitly_out_of_scope_blocks")
    if oos:
        out.append("## Explicitly out of scope")
        out.append("")
        out.append(oos["description"])
        out.append("")
        for e in oos["entries"]:
            out.append(f"- `{e['path']}` — {e['reason']}")
        out.append("")

    MD_PATH.write_text("\n".join(out) + "\n")
    print(f"wrote {MD_PATH}")


if __name__ == "__main__":
    main()
