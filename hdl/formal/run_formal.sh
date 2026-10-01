#!/usr/bin/env bash
# run_formal.sh — run every formal harness under hdl/formal/
#
# smtbmc jobs run all tasks of their .sby file (prove + cover); the
# bootstrap_detector job runs through prove_pdr.sh (see its header). Each
# job has its own wall-clock limit (FORMAL_TIMEOUT seconds, default 900);
# running out of time is a failure, never a pass.
#
# Work directories and logs go to FORMAL_BUILD (default hdl/build/formal).
# Prints PASS/FAIL per job and a summary; exits nonzero if any job fails.
#
# Usage: hdl/formal/run_formal.sh [job ...]   (default: every job below)
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
export FORMAL_BUILD="${FORMAL_BUILD:-$(cd .. && pwd)/build/formal}"
mkdir -p "$FORMAL_BUILD"

SBY_JOBS=(axi4s_skid_buffer udiv_seq cordic cp_removal timing_recovery fft_engine)
PDR_JOBS=(bootstrap_detector)
jobs=("${SBY_JOBS[@]}" "${PDR_JOBS[@]}")
[[ $# -gt 0 ]] && jobs=("$@")

limit="${FORMAL_TIMEOUT:-900}"
pass=0
fail=0
for job in "${jobs[@]}"; do
    log="$FORMAL_BUILD/$job.log"
    start=$SECONDS
    if [[ " ${PDR_JOBS[*]} " == *" $job "* ]]; then
        cmd=(./prove_pdr.sh "$job")
    else
        cmd=(sby -f --prefix "$FORMAL_BUILD/$job" "$job.sby")
    fi
    if timeout "$limit" "${cmd[@]}" >"$log" 2>&1; then
        echo "PASS formal $job ($((SECONDS - start)) s)"
        pass=$((pass + 1))
    else
        rc=$?
        reason="see ${log}"
        [[ $rc -eq 124 ]] && reason="timed out after ${limit} s; $reason"
        echo "FAIL formal $job ($reason)"
        grep -E 'FAIL|failed|Assert|UNDECIDED|ERROR' "$log" | head -5 | sed 's/^/    /' || true
        fail=$((fail + 1))
    fi
done

echo "formal: $pass passed, $fail failed"
[[ $pass -gt 0 && $fail -eq 0 ]]
