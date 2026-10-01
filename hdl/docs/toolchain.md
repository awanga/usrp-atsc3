# HDL Toolchain

> Toolchain pinning + setup, verified end-to-end
> against `hdl/rtl/common/axi4s_skid_buffer.v` (lint → formal → cocotb
> simulation, all passing) before any algorithmic RTL was written.

## Versions verified against

| Tool | Version verified | Install |
|---|---|---|
| Verilator | 5.020 (Debian 5.020-1) | `apt install verilator` |
| Icarus Verilog | 12.0 | `apt install iverilog` |
| Yosys | 0.33 | `apt install yosys` |
| SymbiYosys (`sby`) | 0.68 | `apt install sby` (pulled in with yosys on Debian/Ubuntu) |
| SMT solver | z3 4.8.12 (cvc5 1.1.2 qualified but slower; see formal_conventions.md) | `apt install z3` |
| cocotb | **1.9.2**, pinned -- see below | `pip install -r hdl/sim/cocotb/requirements.txt` inside the venv |
| Python | 3.12.3 | system |

No vendor/proprietary tools anywhere in this list.

### Why cocotb is pinned to 1.9.2, not latest (2.x)

cocotb 2.0.1's bundled Verilator VPI shim
(`cocotb/share/lib/verilator/verilator.cpp`) calls
`VerilatedVpi::clearEvalNeeded()` / `doInertialPuts()` / `evalNeeded()`,
which don't exist in Verilator 5.020 (they were added in a later
Verilator release than what's available via apt on this system). Building
against cocotb 2.0.1 fails at the `g++` step with "is not a member of
`VerilatedVpi`" before any simulation runs. cocotb 1.9.2 uses an older VPI
shim compatible with 5.020 and was verified to build and run cleanly.

Also note: cocotb 2.x renamed the Python runner module from
`cocotb.runner` to `cocotb_tools.runner`. Any future re-attempt at
upgrading cocotb needs that import path updated in
`hdl/sim/cocotb/test_runner.py` alongside re-verifying the Verilator VPI
shim against whatever Verilator version is installed at the time.

## Python environment

The system Python (3.12, Debian) is externally-managed (PEP 668) and
refuses `pip install` outside a venv. Toolchain Python dependencies live
in a project-local venv, not system-wide:

```bash
python3 -m venv hdl/sim/.venv
hdl/sim/.venv/bin/pip install -r hdl/sim/cocotb/requirements.txt
```

`hdl/sim/.venv/` is gitignored (see `.gitignore`); every contributor (and
CI) creates their own from `requirements.txt`, which pins exact versions
so the environment is reproducible.

## Running each stage

Every stage writes logs under the gitignored `hdl/build/`, prints
PASS/FAIL per unit with a count, and exits nonzero on any failure:

```bash
hdl/run_all.sh [--mutants]     # everything below, one summary line per stage

hdl/synth/lint.sh              # Verilator (1364-2001 and 1800-2017) + Icarus -g2001, per file
hdl/synth/synth.sh [top ...]   # technology-independent Yosys synthesis per top
hdl/formal/run_formal.sh [job ...]
hdl/sim/run_sim.sh [-k expr]   # cocotb on Verilator and Icarus; SIM_JOBS, HDL_FULL=1 (nightly sizes)
hdl/mutants/run_mutants.py [-j N] [name ...]
hdl/formal/qualify/qualify_solvers.sh
```

The cocotb benches and the twiddle-ROM regeneration need the fixed-point
golden CLIs: `cmake --build build-fxp` with `-DATSC3_FIXED_POINT=ON
-DATSC3_ENABLE_HDL_STUBS=ON`.

Generated RTL data:
- `hdl/rtl/ofdm/fft_twiddles.hex`: `build-fxp/hdl/sim/golden/fft_twiddle_gen > hdl/rtl/ofdm/fft_twiddles.hex`
  (loaded with `$readmemh`; tools get its absolute path through the
  `TWIDDLE_HEX` parameter).
- `hdl/rtl/sync/timing_recovery_coeffs.vh`: from `echo ROM |
  build-fxp/hdl/sim/golden/timing_recovery_gen`; `test_polyphase_fir.py`
  fails if it drifts from the running filter design.

See `hdl/docs/formal_conventions.md` for the formal harness pattern.

## Simulator timing idiom (cocotb)

Read DUT outputs only where every update of the timestep has landed:
after `await ReadOnly()`, or at the falling edge when inputs are driven
1 ns after the rising edge. Never read sibling outputs straight after
`RisingEdge(<a DUT output>)`: Icarus delivers that callback before the
other non-blocking updates of the timestep, Verilator after, so such a
read races (this is what made the bootstrap bench fail on Icarus only).
A disagreement between the two simulators is a race to root-cause, not a
simulator to pick.

## Formatting and linting

- Python (cocotb benches, mutant runner): the repo's pre-commit hooks,
  black and flake8 (max line length 100).
- Shell: shellcheck.
- Verilog: the lint gate above. Verible is installed but not used: its
  formatter and linter cannot parse files whose port lists use the
  `include`d AXI4-S macros, and its default lint rules require
  SystemVerilog-only syntax (typed parameters, `[N]` unpacked ranges)
  that is illegal in Verilog-2001.

## CI

Not wired yet: `ci.yml` builds on ubuntu-22.04, whose packaged
Verilator and Yosys are older than the versions above, and SymbiYosys
0.68 is installed from source here. An HDL job needs an ubuntu-24.04
runner (or the CI container) with these exact versions, plus a
fixed-point build for the golden CLIs; `hdl/run_all.sh` is the command it
should run.
