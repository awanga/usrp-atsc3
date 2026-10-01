#!/usr/bin/env bash
# run_all.sh — the full HDL gate: lint, synthesis, formal, simulation
#
# Runs every stage even if an earlier one fails, prints each stage's
# summary line and exits nonzero if any stage failed. --mutants also runs
# the committed RTL mutants (slow: each kill reruns a testbench or proof).
# Needs the fixed-point golden CLIs built (cmake --build build-fxp).
#
# Usage: hdl/run_all.sh [--mutants]
set -uo pipefail

HDL="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="$HDL/build"
mkdir -p "$LOG_DIR"

stages=(synth/lint.sh synth/synth.sh formal/run_formal.sh sim/run_sim.sh)
[[ "${1:-}" == "--mutants" ]] && stages+=(mutants/run_mutants.py)

failed=0
for stage in "${stages[@]}"; do
    log="$LOG_DIR/$(basename "${stage%.*}").log"
    if "$HDL/$stage" >"$log" 2>&1; then
        status=PASS
    else
        status=FAIL
        failed=$((failed + 1))
    fi
    printf '%-4s %-24s %s\n' "$status" "$stage" "$(tail -1 "$log")"
done

echo "hdl: $((${#stages[@]} - failed)) of ${#stages[@]} stages passed"
[[ $failed -eq 0 ]]
