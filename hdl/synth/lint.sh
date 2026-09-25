#!/usr/bin/env bash
# lint.sh — Verilog-2001 lint gate for hdl/rtl/
#
# Zero-warning gate: any Verilator warning fails the build (-Wall -Werror-*
# would be one route, but Verilator's granular -Wno-* flags don't have a
# single --Werror-all; instead we fail on any stderr output from
# --lint-only, which is what a clean run produces none of).
#
# Usage: hdl/synth/lint.sh
set -euo pipefail

HDL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RTL_DIR="$HDL_ROOT/rtl"
INCLUDE_DIR="$RTL_DIR/include"

status=0
shopt -s globstar nullglob

# -y/+incdir+ for every immediate rtl/ subdirectory, not just common/: a
# block can instantiate a shared core from rtl/common/ (cordic.v,
# udiv_seq.v) or a same-directory sub-module (rtl/sync/polyphase_fir.v),
# and `` `include `` a same-directory generated file (e.g.
# rtl/sync/timing_recovery_coeffs.vh) -- all while still linting one file
# at a time.
search_args=("+incdir+$INCLUDE_DIR")
for d in "$RTL_DIR"/*/; do
    d="${d%/}"
    search_args+=("-y" "$d" "+incdir+$d")
done

for f in "$RTL_DIR"/**/*.v; do
    echo "linting: ${f#"$HDL_ROOT"/}"
    if ! verilator --lint-only --language 1364-2001 -Wall "${search_args[@]}" "$f"; then
        status=1
    fi
done

if [[ $status -ne 0 ]]; then
    echo "lint: FAILED" >&2
    exit 1
fi
echo "lint: all rtl/ files clean"
