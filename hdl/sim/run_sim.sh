#!/usr/bin/env bash
# run_sim.sh — run every cocotb testbench on Verilator and Icarus
#
# One job per (block or integration bench, simulator), collected from
# hdl/sim/cocotb/test_runner.py. Each job logs to hdl/build/sim/logs/;
# prints PASS/FAIL per job and a summary; exits nonzero if any job fails.
# SIM_JOBS sets how many jobs run in parallel (default 4). HDL_FULL=1 adds
# the full-size (nightly) scenarios.
#
# Usage: hdl/sim/run_sim.sh [pytest -k expression]
set -uo pipefail

SIM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HDL_ROOT="$(dirname "$SIM_DIR")"
PY="$SIM_DIR/.venv/bin/python"
LOG_DIR="$HDL_ROOT/build/sim/logs"
mkdir -p "$LOG_DIR"
cd "$SIM_DIR/cocotb" || exit 1

collect=("$PY" -m pytest test_runner.py --collect-only -q -p no:cacheprovider)
[[ $# -gt 0 ]] && collect+=(-k "$1")
mapfile -t ids < <("${collect[@]}" 2>/dev/null | grep '::')
if [[ ${#ids[@]} -eq 0 ]]; then
    echo "run_sim: no testbench jobs selected" >&2
    exit 1
fi

run_one() {
    local id="$1" log
    log="$LOG_DIR/$(tr -c 'A-Za-z0-9_.-' '_' <<<"${id#*::}").log"
    if "$PY" -m pytest "$id" -q -s -p no:cacheprovider >"$log" 2>&1; then
        echo "PASS sim ${id#*::}"
    else
        echo "FAIL sim ${id#*::} (see ${log#"$HDL_ROOT"/})"
    fi
}
export -f run_one
export PY LOG_DIR HDL_ROOT

# $1 must expand in the child bash, not here.
# shellcheck disable=SC2016
results="$(printf '%s\n' "${ids[@]}" | xargs -P "${SIM_JOBS:-4}" -I{} bash -c 'run_one "$1"' _ {})"
sort <<<"$results"
pass=$(grep -c '^PASS' <<<"$results")
fail=$(grep -c '^FAIL' <<<"$results")
echo "sim: $pass passed, $fail failed"
[[ $pass -gt 0 && $fail -eq 0 ]]
