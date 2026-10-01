#!/usr/bin/env bash
# qualify_solvers.sh — check which formal engines are usable here
#
# For each engine: the trivial harness in qualify.v must PASS when correct
# and produce a counterexample when broken, each within 60 s. A crash, a
# missing status or a timeout disqualifies the engine; record the result
# in hdl/docs/toolchain.md rather than retrying.
#
# Logs go to hdl/build/formal/qualify/. Usage: hdl/formal/qualify/qualify_solvers.sh
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1
out="$(cd ../.. && pwd)/build/formal/qualify"
mkdir -p "$out"

ENGINES=("smtbmc z3" "smtbmc cvc5" "smtbmc boolector" "smtbmc bitwuzla" "smtbmc yices"
    "abc pdr" "abc bmc3" "direct pdr" "direct bmc3")

# "direct <cmd>": sby only builds the AIG (its abc engine crashes after
# solving, see toolchain.md), then ABC runs <cmd> on it -- the
# prove_pdr.sh flow.
run_case() { # engine should_fail -> prints PASS|FAIL|ERROR|TIMEOUT
    local engine="$1" fail="$2" name mode=prove sby_engine="$1"
    name="$(tr ' ' '_' <<<"$engine")_$fail"
    [[ $engine == *bmc3 ]] && mode=bmc
    [[ $engine == direct* ]] && sby_engine="abc pdr" && mode=prove
    cat >"$out/$name.sby" <<SBY
[options]
mode $mode
depth 8

[engines]
$sby_engine

[script]
read_verilog -formal qualify.v
chparam -set SHOULD_FAIL $fail qualify
prep -top qualify

[files]
$PWD/qualify.v
SBY
    local rc=0
    timeout 60 sby -f "$out/$name.sby" >"$out/$name.log" 2>&1 || rc=$?
    if [[ $engine == direct* ]]; then
        local aig="$out/$name/model/design_aiger.aig" abc_out
        if [[ ! -f $aig ]]; then
            echo ERROR
            return
        fi
        rc=0
        abc_out="$(timeout 60 yosys-abc -c "read_aiger $aig; fold; strash; ${engine#direct } \
            $([[ $engine == *bmc3 ]] && echo '-F 20')" 2>&1)" || rc=$?
        echo "$abc_out" >>"$out/$name.log"
        if [[ $rc -eq 124 ]]; then
            echo TIMEOUT
        elif grep -q 'Property proved' <<<"$abc_out"; then
            echo PASS
        elif grep -q 'asserted in frame' <<<"$abc_out"; then
            echo FAIL
        elif [[ $engine == *bmc3 ]] && grep -qE 'No output asserted|Explored all reachable' <<<"$abc_out"; then
            echo PASS  # bounded: no counterexample within the frame limit
        else
            echo ERROR
        fi
        return
    fi
    if [[ $rc -eq 124 ]]; then
        echo TIMEOUT
    elif grep -q 'DONE (PASS' "$out/$name.log"; then
        echo PASS
    elif grep -q 'DONE (FAIL' "$out/$name.log"; then
        echo FAIL
    else
        echo ERROR
    fi
}

for engine in "${ENGINES[@]}"; do
    start=$SECONDS
    good="$(run_case "$engine" 0)"
    bad="$(run_case "$engine" 1)"
    verdict=UNUSABLE
    [[ $good == PASS && $bad == FAIL ]] && verdict=QUALIFIED
    printf '%-10s %-18s correct=%-7s broken=%-7s (%d s)\n' "$verdict" "$engine" "$good" "$bad" \
        $((SECONDS - start))
done
