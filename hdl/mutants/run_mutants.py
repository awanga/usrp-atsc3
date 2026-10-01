#!/usr/bin/env python3
"""Apply each RTL mutant from manifest.py to a private copy of hdl/rtl/
and check that every checker listed for it fails.

Results per (mutant, checker): KILLED (checker failed, as required; a
formal kill without a counterexample -- the proof merely stopped closing
-- is labelled as such),
SURVIVED (checker passed -- the checker is too weak), STALE (the
mutation no longer applies to the RTL) or ERROR (the checker could not
run, e.g. a build error, so the result proves nothing). Exits nonzero
unless every pair is KILLED.

Work directories and logs: hdl/build/mutants/<mutant>/.

Usage: hdl/mutants/run_mutants.py [-j N] [--list] [name-or-block ...]
"""

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
from pathlib import Path

HDL = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HDL / "mutants"))
from manifest import MUTANTS  # noqa: E402

BUILD = HDL / "build" / "mutants"
PYTHON = HDL / "sim" / ".venv" / "bin" / "python"
SIM_TIMEOUT = int(os.environ.get("MUTANT_SIM_TIMEOUT", "3600"))
FORMAL_TIMEOUT = int(os.environ.get("MUTANT_FORMAL_TIMEOUT", "1800"))


def prepare(m):
    ws = BUILD / m["name"]
    shutil.rmtree(ws, ignore_errors=True)
    shutil.copytree(HDL / "rtl", ws / "rtl")
    (ws / "formal").mkdir(parents=True)
    for f in (HDL / "formal").iterdir():
        if f.is_file() and (f.suffix in (".sby", ".v", ".sh")):
            shutil.copy2(f, ws / "formal" / f.name)
    target = ws / "rtl" / m["file"]
    text = target.read_text()
    if text.count(m["find"]) != 1:
        return (
            ws,
            f"STALE: find text occurs {text.count(m['find'])} times in {m['file']}",
        )
    target.write_text(text.replace(m["find"], m["replace"]))
    return ws, None


def run(cmd, log, env=None, cwd=None, timeout=None):
    with open(log, "w") as fh:
        try:
            p = subprocess.run(
                cmd,
                stdout=fh,
                stderr=subprocess.STDOUT,
                cwd=cwd,
                env={**os.environ, **(env or {})},
                timeout=timeout,
            )
            rc = p.returncode
        except subprocess.TimeoutExpired:
            rc = None
    return rc, Path(log).read_text(errors="replace")


def check(ws, killer):
    kind, target = killer.split(":")
    log = ws / f"{kind}_{target}.log"
    if kind == "sim":
        rc, out = run(
            [
                str(PYTHON),
                "-m",
                "pytest",
                "-q",
                "-p",
                "no:cacheprovider",
                f"test_runner.py::test_block[{target}-verilator]",
            ],
            log,
            env={"HDL_RTL_DIR": str(ws / "rtl"), "HDL_SIM_BUILD": str(ws / "sim")},
            cwd=HDL / "sim" / "cocotb",
            timeout=SIM_TIMEOUT,
        )
        if rc is None:
            return "ERROR (timeout)"
        if rc == 0:
            return "SURVIVED"
        # A failing cocotb test shows up as a FAIL row in cocotb's summary;
        # anything else (build error, no tests collected) proves nothing.
        return "KILLED" if " FAIL " in out else "ERROR (see log)"
    if kind == "formal":
        rc, out = run(
            [
                "sby",
                "-f",
                "--prefix",
                str(ws / "formal_build" / target),
                f"{target}.sby",
            ],
            log,
            cwd=ws / "formal",
            timeout=FORMAL_TIMEOUT,
        )
        if rc is None:
            return "ERROR (timeout)"
        if rc == 0:
            return "SURVIVED"
        if "DONE (FAIL" in out:
            return "KILLED"
        if "DONE (UNKNOWN" in out:
            # k-induction no longer closes and BMC found no counterexample
            # within the harness depth (the bug sits behind deeper events):
            # the proof depends on the mutated logic, but no trace was shown.
            return "KILLED (induction only, no counterexample within depth)"
        return "ERROR (see log)"
    if kind == "bmc":
        rc, out = run(
            [str(ws / "formal" / "prove_pdr.sh"), "--mutant", target],
            log,
            env={"FORMAL_BUILD": str(ws / "formal_build")},
            timeout=FORMAL_TIMEOUT,
        )
        if rc == 0 and "mutant caught" in out:
            return "KILLED"
        return "SURVIVED" if rc == 2 else "ERROR (see log)"
    return f"ERROR (unknown checker kind {kind})"


def evaluate(m):
    ws, stale = prepare(m)
    if stale:
        return [(m["name"], k, stale) for k in m["killers"]]
    return [(m["name"], k, check(ws, k)) for k in m["killers"]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-j", type=int, default=1, help="mutants evaluated in parallel")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("select", nargs="*", help="mutant names or substrings of them")
    args = ap.parse_args()

    chosen = [
        m
        for m in MUTANTS
        if not args.select or any(s in m["name"] for s in args.select)
    ]
    if args.list:
        for m in chosen:
            print(f"{m['name']:34s} {m['file']:28s} {' '.join(m['killers'])}")
        return 0
    if not chosen:
        print("run_mutants: no mutants selected", file=sys.stderr)
        return 1

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.j) as pool:
        for rows in pool.map(evaluate, chosen):
            for name, killer, verdict in rows:
                print(
                    f"{verdict.split()[0]:8s} {name:34s} {killer}"
                    + (f"  [{verdict}]" if " " in verdict else ""),
                    flush=True,
                )
                results.append(verdict)
    killed = sum(r.startswith("KILLED") for r in results)
    print(f"mutants: {killed} of {len(results)} mutant/checker pairs killed")
    return 0 if results and killed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
