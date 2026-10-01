#!/usr/bin/env python3
"""Generate lib/ofdm/nuc_tables.h from the ATSC A/322 PDF (Annex C).

The non-uniform constellation (NUC) position vectors and the 1D-NUC bit
labelling are read directly from the standard's Annex C:
  Table C.1.2 - C.1.7   2D-NUC position vectors w (16/64/256QAM)
  Table C.1.8 - C.1.11  1D-NUC position vectors u (1024/4096QAM)
  Table C.3.1 - C.3.4   1D-NUC bit labelling (real and imaginary parts)

Usage:
  scripts/gen_nuc_tables.py A322-2018-Physical-Layer-Protocol.pdf \
      > lib/ofdm/nuc_tables.h

The PDF is published by ATSC at
https://www.atsc.org/wp-content/uploads/2021/04/A322-2018-Physical-Layer-Protocol.pdf
and is not redistributed here. Requires pypdf (BSD-3-Clause; 6.13.0 used).

Every table is checked for completeness (row count, column count, value
range); the real/imaginary labellings must agree wherever both extract
cleanly. Any failure exits nonzero rather than emitting a partial header.
"""

import re
import sys

import pypdf

RATES = [f"{n}/15" for n in range(2, 14)]

# (modulation, table pair covering rates 2-7/15 and 8-13/15, row count)
TWO_D = [
    (16, "C.1.2", "C.1.3", 4),
    (64, "C.1.4", "C.1.5", 16),
    (256, "C.1.6", "C.1.7", 64),
]
ONE_D = [(1024, "C.1.8", "C.1.9", 16), (4096, "C.1.10", "C.1.11", 32)]
LABELS = {1024: ("C.3.1", "C.3.2"), 4096: ("C.3.3", "C.3.4")}

COMPLEX_RE = re.compile(r"^(-?\d+\.\d+)([+-])j(\d+\.\d+)$")


def fail(msg):
    sys.exit(f"gen_nuc_tables: {msg}")


def annex_c_lines(pdf_path):
    text = "\n".join(
        page.extract_text() or "" for page in pypdf.PdfReader(pdf_path).pages
    )
    lines = [ln.strip() for ln in text.split("\n")]
    # The table of contents lists the same captions followed by dot leaders.
    start = next(
        (
            i
            for i, ln in enumerate(lines)
            if ln.startswith("Table C.1.1 ") and "...." not in ln
        ),
        None,
    )
    if start is None:
        fail("Table C.1.1 not found; is this the A/322 PDF?")
    end = next(
        (i for i in range(start, len(lines)) if lines[i].startswith("Table D.")),
        None,
    )
    if end is None:
        fail("end of Annex C (Table D.*) not found")
    return lines[start:end]


def split_tables(lines):
    """Map table id -> its body lines (page headers included; rows are
    selected by prefix later, so they are harmless)."""
    tables = {}
    current = None
    for ln in lines:
        m = re.match(r"^Table (C\.\d+\.\d+) ", ln)
        if m:
            current = m.group(1)
            tables[current] = []
        elif ln.startswith("C.2 CONSTELLATION FIGURES"):
            current = None
        elif current is not None:
            tables[current].append(ln)
    return tables


def parse_complex(tok):
    m = COMPLEX_RE.match(tok)
    if not m:
        fail(f"bad complex value {tok!r}")
    re_part = float(m.group(1))
    im_part = float(m.group(3)) * (1 if m.group(2) == "+" else -1)
    return re_part, im_part


def position_rows(body, prefix, count, table_id):
    rows = {}
    for ln in body:
        toks = ln.split()
        if toks and re.fullmatch(rf"{prefix}\d+", toks[0]):
            k = int(toks[0][1:])
            if k in rows:
                fail(f"Table {table_id}: duplicate row {toks[0]}")
            rows[k] = toks[1:]
    if sorted(rows) != list(range(count)):
        fail(
            f"Table {table_id}: rows {sorted(rows)}, expected {prefix}0..{prefix}{count - 1}"
        )
    for k, vals in rows.items():
        if len(vals) != 6:
            fail(f"Table {table_id} {prefix}{k}: {len(vals)} columns, expected 6")
    return [rows[k] for k in range(count)]


def two_d_table(tables, lo, hi, count):
    per_rate = [[] for _ in RATES]
    for half, tid in enumerate((lo, hi)):
        for row in position_rows(tables[tid], "w", count, tid):
            for col, tok in enumerate(row):
                per_rate[half * 6 + col].append(parse_complex(tok))
    for vals in per_rate:
        for re_part, im_part in vals:
            if not (0.0 < re_part < 2.0 and 0.0 < im_part < 2.0):
                fail(f"2D-NUC point {re_part}+j{im_part} outside the first quadrant")
    return per_rate


def one_d_table(tables, lo, hi, count):
    per_rate = [[] for _ in RATES]
    for half, tid in enumerate((lo, hi)):
        for row in position_rows(tables[tid], "u", count, tid):
            for col, tok in enumerate(row):
                val = float(tok)
                if not 0.0 < val < 2.0:
                    fail(f"Table {tid}: amplitude {val} out of range")
                per_rate[half * 6 + col].append(val)
    return per_rate


def label_blocks(body, table_id, bits):
    """Parse a labelling table into its blocks of 16 points. Each block is
    a dict label -> u index (label = the axis bits, sign bit first), or
    None where the PDF text of a bit row is damaged."""
    blocks = []
    bit_rows = []
    damaged = False
    for ln in body:
        toks = ln.split()
        if not toks:
            continue
        if re.fullmatch(r"y\d+,s", toks[0]):
            vals = toks[1:]
            if len(vals) == 16 and not set(vals) - {"0", "1"}:
                bit_rows.append([int(v) for v in vals])
            else:
                damaged = True
        elif toks[0] in ("Re(zs)", "Im(zs)"):
            if len(toks) != 17:
                fail(f"Table {table_id}: point row with {len(toks) - 1} entries")
            if damaged or len(bit_rows) != bits:
                blocks.append(None)
            else:
                block = {}
                for col, tok in enumerate(toks[1:]):
                    m = re.fullmatch(r"(-?)u(\d+)", tok)
                    if not m:
                        fail(f"Table {table_id}: bad point {tok!r}")
                    v = 0
                    for row in bit_rows:
                        v = (v << 1) | row[col]
                    if (m.group(1) == "-") != bool(v >> (bits - 1)):
                        fail(
                            f"Table {table_id}: sign of {tok} disagrees with the sign bit"
                        )
                    block[v] = int(m.group(2))
                blocks.append(block)
            bit_rows = []
            damaged = False
    return blocks


def label_map(tables, re_id, im_id, bits):
    """map[v] = u index. A/322 applies the same labelling to both axes, so
    each block is taken from whichever of the two tables extracted cleanly
    (the PDF misplaces a few cells in each), and blocks clean in both must
    agree."""
    re_blocks = label_blocks(tables[re_id], re_id, bits)
    im_blocks = label_blocks(tables[im_id], im_id, bits)
    if len(re_blocks) != len(im_blocks):
        fail(f"{re_id}/{im_id}: {len(re_blocks)} vs {len(im_blocks)} blocks")
    mapping = [None] * (1 << bits)
    for n, (a, b) in enumerate(zip(re_blocks, im_blocks)):
        if a is None and b is None:
            fail(f"{re_id}/{im_id}: block {n} damaged in both tables")
        if a is not None and b is not None and a != b:
            fail(f"{re_id}/{im_id}: block {n} labellings differ")
        for v, u in (a or b).items():
            if mapping[v] is not None:
                fail(f"{re_id}/{im_id}: label {v:0{bits}b} defined twice")
            mapping[v] = u
    if None in mapping:
        fail(f"{re_id}/{im_id}: labels missing for {mapping.count(None)} values")
    return mapping


def fmt(v):
    return f"{v:.4f}f"


def emit(two_d, one_d, maps):
    out = []
    w = out.append
    w("#pragma once")
    w("")
    w("// Non-uniform constellation (NUC) tables, ATSC A/322:2018 Annex C.")
    w("//")
    w("// Generated by scripts/gen_nuc_tables.py from the standard's PDF; do not")
    w("// edit by hand. Values are the published position vectors, before power")
    w("// normalization:")
    w("//   2D-NUC (16/64/256QAM, Tables C.1.2-C.1.7): first-quadrant points w,")
    w("//     expanded to the full constellation by A/322 Section 6.3.4.2.")
    w("//   1D-NUC (1024/4096QAM, Tables C.1.8-C.1.11): per-axis amplitudes u,")
    w("//     labelled per Tables C.3.1-C.3.4 (Section 6.3.4.3).")
    w("")
    w("#include <cstddef>")
    w("#include <cstdint>")
    w("")
    w("namespace atsc3 {")
    w("namespace ofdm {")
    w("")
    w("// Code rates 2/15 through 13/15, indexed by config::CodeRate")
    w(f"constexpr size_t NUM_CODE_RATES = {len(RATES)};")
    for mod, per_rate in two_d:
        b = mod // 4
        w("")
        w("// " + "=" * 77)
        w(f"// {mod}-NUC: {b} first-quadrant points per code rate")
        w("// " + "=" * 77)
        w("// Quadrant expansion (A/322 Section 6.3.4.2), x = full constellation:")
        w(f"//   x[0..{b - 1}] = w, x[{b}..{2 * b - 1}] = -conj(w),")
        w(f"//   x[{2 * b}..{3 * b - 1}] = conj(w), x[{3 * b}..{4 * b - 1}] = -w")
        w("")
        w(f"constexpr size_t NUC_{mod}_BASE_POINTS = {b};")
        w("")
        w("// clang-format off")
        w(
            f"constexpr float NUC_{mod}_TABLE[NUM_CODE_RATES][NUC_{mod}_BASE_POINTS][2] = {{"
        )
        for r, vals in enumerate(per_rate):
            w(f"    // Rate {RATES[r]} (index {r})")
            pts = [f"{{{fmt(a)}, {fmt(c)}}}" for a, c in vals]
            w("    {")
            for i in range(0, len(pts), 4):
                sep = "," if i + 4 < len(pts) else ""
                w("        " + ", ".join(pts[i : i + 4]) + sep)
            w("    }" + ("," if r + 1 < len(per_rate) else ""))
        w("};")
        w("// clang-format on")
    for mod, per_rate in one_d:
        n = len(per_rate[0])
        side = 2 * n
        bits = side.bit_length() - 1
        w("")
        w("// " + "=" * 77)
        w(f"// {mod}-NUC: 1D separable constellation, {n} amplitudes per code rate")
        w("// " + "=" * 77)
        w("")
        w(f"constexpr size_t NUC_{mod}_AMPLITUDES = {n};")
        w("")
        w("// clang-format off")
        w(f"constexpr float NUC_{mod}_TABLE[NUM_CODE_RATES][NUC_{mod}_AMPLITUDES] = {{")
        for r, vals in enumerate(per_rate):
            w(f"    // Rate {RATES[r]} (index {r})")
            amps = [fmt(v) for v in vals]
            w("    {")
            for i in range(0, len(amps), 8):
                sep = "," if i + 8 < len(amps) else ""
                w("        " + ", ".join(amps[i : i + 8]) + sep)
            w("    }" + ("," if r + 1 < len(per_rate) else ""))
        w("};")
        w("")
        w(f"// Per-axis label -> amplitude index. The label is the {bits} bits of one")
        w("// axis (y1,y3,... for Re, y0,y2,... for Im; first bit = MSB = sign,")
        w("// 1 = negative); the same labelling applies to both axes.")
        w(f"constexpr int NUC_{mod}_MAP[{side}] = {{")
        m = [str(v) for v in maps[mod]]
        for i in range(0, side, 16):
            sep = "," if i + 16 < side else ""
            w("    " + ", ".join(m[i : i + 16]) + sep)
        w("};")
        w("// clang-format on")
    w("")
    w("// Get code rate index from CodeRate enum")
    w("inline size_t code_rate_index(uint8_t rate_enum) {")
    w("    return static_cast<size_t>(rate_enum);")
    w("}")
    w("")
    w("}  // namespace ofdm")
    w("}  // namespace atsc3")
    return "\n".join(out) + "\n"


def main():
    if len(sys.argv) != 2:
        fail("usage: gen_nuc_tables.py <A322 PDF>")
    tables = split_tables(annex_c_lines(sys.argv[1]))
    two_d = [(mod, two_d_table(tables, lo, hi, n)) for mod, lo, hi, n in TWO_D]
    one_d = [(mod, one_d_table(tables, lo, hi, n)) for mod, lo, hi, n in ONE_D]
    maps = {}
    for mod, (re_id, im_id) in LABELS.items():
        maps[mod] = label_map(tables, re_id, im_id, (mod.bit_length() - 1) // 2)
    sys.stdout.write(emit(two_d, one_d, maps))


if __name__ == "__main__":
    main()
