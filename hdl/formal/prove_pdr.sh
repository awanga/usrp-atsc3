#!/usr/bin/env bash
# prove_pdr.sh — unbounded proof of a .sby job's `prove` task with ABC PDR,
# or a bounded counterexample search for mutant (non-vacuity) checks
#
# For harnesses whose datapath is too heavy for smtbmc/z3 (bootstrap_detector:
# z3 cannot finish even a depth-2 BMC, while PDR proves it in under a
# second). The job's .sby file stays the single source of truth for the
# model: sby builds the AIG exactly as it would for its own `abc pdr`
# engine, then this script runs ABC itself.
#
# Why not let sby run PDR: SBY 0.68's ABC-engine result parser expects an
# "asserts" key in the witness map that Yosys 0.33's write_aiger does not
# emit, so sby crashes (KeyError: 'asserts') after PDR has already
# finished. See hdl/docs/toolchain.md. sby's own exit status is therefore
# ignored, but only after checking that its AIG stage finished cleanly in a
# fresh workdir -- a model-generation failure still fails this script.
#
# Both modes run under a time limit and report running out of it as
# UNDECIDED (exit 2), never as a pass and never as a hang:
#   default   PDR proof (PDR_TIMEOUT seconds, default 1800). PDR is a
#             proof engine; it finds shallow counterexamples quickly but
#             stalls on deep ones.
#   --mutant  bounded BMC (bmc3, up to BMC_FRAMES frames, default 600, and
#             BMC_TIMEOUT seconds, default 1800) for checking that a
#             deliberately broken RTL *fails*. Use this, not PDR, for
#             mutants: e.g. a bootstrap_detector bug that needs two full
#             samples (~160 frames) ran PDR for 14 hours without a result,
#             while bmc3 found it in under two minutes.
#
# Usage: hdl/formal/prove_pdr.sh [--mutant] <job>   (e.g. bootstrap_detector)
set -euo pipefail

mode=prove
if [[ "${1:-}" == "--mutant" ]]; then
    mode=mutant
    shift
fi
job="${1:?usage: prove_pdr.sh [--mutant] <job>}"
cd "$(dirname "${BASH_SOURCE[0]}")"
workdir="${job}_prove"
log="$job.prove_pdr.log"

rm -rf "$workdir"
sby -f "$job.sby" prove > "$log" 2>&1 || true

if ! grep -q '\] aig: finished (returncode=0)' "$log"; then
    echo "prove_pdr: sby did not produce the AIG model; see hdl/formal/$log" >&2
    exit 1
fi

aig="$workdir/model/design_aiger.aig"
if [[ "$mode" == prove ]]; then
    abc_cmd="read_aiger $aig; fold; strash; pdr -T ${PDR_TIMEOUT:-1800}"
else
    abc_cmd="read_aiger $aig; fold; strash; bmc3 -F ${BMC_FRAMES:-600} -T ${BMC_TIMEOUT:-1800}"
fi
abc_out="$(yosys-abc -c "$abc_cmd" 2>&1)"
echo "$abc_out" >> "$log"

cex="$(grep -E 'asserted in frame' <<< "$abc_out" || true)"
if [[ "$mode" == prove ]]; then
    if grep -q 'Property proved' <<< "$abc_out"; then
        echo "prove_pdr: $job PASS (all assertions proved, unbounded)"
        rm -f "$log"
    elif [[ -n "$cex" ]]; then
        echo "prove_pdr: $job FAIL: $cex" >&2
        echo "full log: hdl/formal/$log" >&2
        exit 1
    else
        echo "prove_pdr: $job UNDECIDED: PDR hit its ${PDR_TIMEOUT:-1800}s limit" >&2
        echo "full log: hdl/formal/$log" >&2
        exit 2
    fi
else
    if [[ -n "$cex" ]]; then
        echo "prove_pdr: $job mutant caught: $cex"
        rm -f "$log"
    else
        echo "prove_pdr: $job mutant NOT caught within ${BMC_FRAMES:-600} frames /" \
             "${BMC_TIMEOUT:-1800}s -- raise the bounds or strengthen the harness" >&2
        echo "full log: hdl/formal/$log" >&2
        exit 2
    fi
fi
