#!/usr/bin/env bash
# prove_pdr.sh — unbounded proof of a .sby job's `prove` task with ABC PDR
#
# For harnesses whose datapath is too heavy for smtbmc/z3 (bootstrap_detector:
# z3 cannot finish even a depth-2 BMC, while PDR proves it in under a
# second). The job's .sby file stays the single source of truth for the
# model: sby builds the AIG exactly as it would for its own `abc pdr`
# engine, then this script runs PDR itself, with the same ABC command sby
# uses.
#
# Why not let sby run PDR: SBY 0.68's ABC-engine result parser expects an
# "asserts" key in the witness map that Yosys 0.33's write_aiger does not
# emit, so sby crashes (KeyError: 'asserts') after PDR has already
# finished. See hdl/docs/toolchain.md. sby's own exit status is therefore
# ignored, but only after checking that its AIG stage finished cleanly in a
# fresh workdir -- a model-generation failure still fails this script.
#
# Usage: hdl/formal/prove_pdr.sh <job>     (e.g. bootstrap_detector)
set -euo pipefail

job="${1:?usage: prove_pdr.sh <job>}"
cd "$(dirname "${BASH_SOURCE[0]}")"
workdir="${job}_prove"

rm -rf "$workdir"
sby -f "$job.sby" prove > "$job.prove_pdr.log" 2>&1 || true

if ! grep -q '\] aig: finished (returncode=0)' "$job.prove_pdr.log"; then
    echo "prove_pdr: sby did not produce the AIG model; see $job.prove_pdr.log" >&2
    exit 1
fi

abc_out="$(yosys-abc -c "read_aiger $workdir/model/design_aiger.aig; fold; strash; pdr" 2>&1)"
echo "$abc_out" >> "$job.prove_pdr.log"

if grep -q 'Property proved' <<< "$abc_out"; then
    echo "prove_pdr: $job PASS (all assertions proved, unbounded)"
    rm -f "$job.prove_pdr.log"
else
    echo "prove_pdr: $job FAIL" >&2
    grep -E 'asserted in frame|Output' <<< "$abc_out" >&2 || true
    echo "full log: hdl/formal/$job.prove_pdr.log" >&2
    exit 1
fi
