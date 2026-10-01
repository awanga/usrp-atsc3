#!/usr/bin/env bash
# synth.sh — technology-independent Yosys synthesis check for every RTL top
#
# Generic gates only (no vendor library): proves each block elaborates and
# synthesizes cleanly and reports its size. A block fails if Yosys errors,
# prints any warning, infers a latch, or `check -assert` finds a
# multiple-driver, undriven or combinational-loop problem, or runs past
# SYNTH_TIMEOUT seconds (default 600). Inferred memories are counted from
# the $mem_v2 cells left after Yosys's coarse pass, before technology
# mapping turns them into gates.
#
# Logs go to hdl/build/synth/<top>.log. Prints PASS/FAIL per top and a
# summary; exits nonzero if any top fails.
#
# Usage: hdl/synth/synth.sh [top ...]   (default: every top below)
set -euo pipefail

HDL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RTL_DIR="${HDL_RTL_DIR:-$HDL_ROOT/rtl}"
LOG_DIR="$HDL_ROOT/build/synth"
mkdir -p "$LOG_DIR"

declare -A SOURCES=(
    [axi4s_skid_buffer]="common/axi4s_skid_buffer.v"
    [udiv_seq]="common/udiv_seq.v"
    [cordic]="common/cordic.v"
    [bootstrap_detector]="sync/bootstrap_detector.v common/cordic.v common/udiv_seq.v"
    [polyphase_fir]="sync/polyphase_fir.v"
    [timing_recovery]="sync/timing_recovery.v sync/polyphase_fir.v"
    [cp_removal]="ofdm/cp_removal.v"
    [fft_engine]="ofdm/fft_engine.v"
)
# Extra Yosys commands run after reading the sources, per top.
declare -A SETUP=(
    [fft_engine]="chparam -set TWIDDLE_HEX \"$RTL_DIR/ofdm/fft_twiddles.hex\" fft_engine;"
)
TOPS=(axi4s_skid_buffer udiv_seq cordic bootstrap_detector polyphase_fir
    timing_recovery cp_removal fft_engine)
[[ $# -gt 0 ]] && TOPS=("$@")

pass=0
fail=0
for top in "${TOPS[@]}"; do
    log="$LOG_DIR/$top.log"
    reads=""
    for s in ${SOURCES[$top]}; do
        reads+="read_verilog -I$RTL_DIR/include -I$(dirname "$RTL_DIR/$s") $RTL_DIR/$s; "
    done
    script="$reads ${SETUP[$top]:-}
        hierarchy -check -top $top;
        synth -flatten -top $top -run :fine;
        select -assert-none t:\$dlatch t:\$adlatch t:\$dlatchsr t:\$sr;
        tee -o $LOG_DIR/$top.mem dump t:\$mem_v2;
        techmap; opt -fast;
        abc -fast -g AND,NAND,OR,NOR,XOR,XNOR,ANDNOT,ORNOT,MUX;
        opt_clean;
        check -assert;
        tee -o $LOG_DIR/$top.stat stat"
    if timeout "${SYNTH_TIMEOUT:-600}" yosys -q -l "$log" -p "$script" >/dev/null 2>&1 &&
        ! grep -q '^Warning' "$log"; then
        cells=$(awk '/Number of cells:/ {n = $4} END {print n}' "$LOG_DIR/$top.stat")
        rambits=$(awk '/parameter .SIZE / {s = $3} /parameter .WIDTH / {t += s * $3} END {print t + 0}' \
            "$LOG_DIR/$top.mem")
        echo "PASS synth $top (gates=$cells, RAM/ROM bits=$rambits)"
        pass=$((pass + 1))
    else
        echo "FAIL synth $top (see ${log#"$HDL_ROOT"/})"
        grep -E '^(Warning|ERROR)' "$log" | head -10 | sed 's/^/    /'
        fail=$((fail + 1))
    fi
done

echo "synth: $pass passed, $fail failed"
[[ $pass -gt 0 && $fail -eq 0 ]]
