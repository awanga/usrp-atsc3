#!/usr/bin/env bash
# lint.sh — Verilog-2001 lint gate for hdl/rtl/
#
# Every rtl/ file must produce no output at all from each of:
#   - verilator --language 1364-2001 -Wall  (no SystemVerilog constructs)
#   - verilator --language 1800-2017 -Wall  (no SystemVerilog keywords used
#     as identifiers -- the formal flow reads RTL as SystemVerilog)
#   - iverilog -g2001 -Wall                 (second frontend: catches
#     net/variable legality errors Verilator accepts, e.g. a reg driven by
#     a continuous assign)
#
# Logs go to hdl/build/lint/<file>.log. Prints PASS/FAIL per file and a
# summary; exits nonzero if any file fails.
#
# Usage: hdl/synth/lint.sh
set -euo pipefail

HDL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RTL_DIR="${HDL_RTL_DIR:-$HDL_ROOT/rtl}"
LOG_DIR="$HDL_ROOT/build/lint"
mkdir -p "$LOG_DIR"

# A block can instantiate a shared core from another rtl/ subdirectory and
# `include a generated file from its own directory, while each file is
# still linted on its own.
vl_args=("+incdir+$RTL_DIR/include")
iv_args=("-I$RTL_DIR/include")
for d in "$RTL_DIR"/*/; do
    d="${d%/}"
    vl_args+=("-y" "$d" "+incdir+$d")
    iv_args+=("-y" "$d" "-I$d")
done

shopt -s globstar nullglob
pass=0
fail=0
for f in "$RTL_DIR"/**/*.v; do
    rel="${f#"$RTL_DIR"/}"
    log="$LOG_DIR/${rel//\//_}.log"
    {
        verilator --lint-only --language 1364-2001 -Wall "${vl_args[@]}" "$f" 2>&1 || echo "verilator-2001: exit $?"
        verilator --lint-only --language 1800-2017 -Wall "${vl_args[@]}" "$f" 2>&1 || echo "verilator-2017: exit $?"
        iverilog -g2001 -Wall "${iv_args[@]}" -o /dev/null "$f" 2>&1 || echo "iverilog: exit $?"
    } >"$log"
    if [[ -s "$log" ]]; then
        echo "FAIL lint $rel (see ${log#"$HDL_ROOT"/})"
        sed 's/^/    /' "$log" | head -20
        fail=$((fail + 1))
    else
        echo "PASS lint $rel"
        pass=$((pass + 1))
    fi
done

echo "lint: $pass passed, $fail failed"
[[ $pass -gt 0 && $fail -eq 0 ]]
