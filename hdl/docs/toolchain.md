# HDL Toolchain

> Phase 9.0 deliverable: toolchain pinning + setup, verified end-to-end
> against `hdl/rtl/common/axi4s_skid_buffer.v` (lint → formal → cocotb
> simulation, all passing) before any algorithmic RTL was written.

## Versions verified against

| Tool | Version verified | Install |
|---|---|---|
| Verilator | 5.020 (Debian 5.020-1) | `apt install verilator` |
| Yosys | 0.33 | `apt install yosys` |
| SymbiYosys (`sby`) | 0.68 | `apt install sby` (pulled in with yosys on Debian/Ubuntu) |
| SMT solver | z3 (boolector also available) | `apt install z3` |
| cocotb | **1.9.2**, pinned -- see below | `pip install -r hdl/sim/cocotb/requirements.txt` inside the venv |
| Python | 3.12.3 | system |

No vendor/proprietary tools anywhere in this list, consistent with
`CLAUDE.md`'s "no vendor DSP primitives" rule extending to the toolchain.

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
per `CLAUDE.md`'s dependency policy.

## Running each stage

```bash
# Lint (Verilog-2001 only, zero warnings gate)
verilator --lint-only --language 1364-2001 -Wall \
    +incdir+hdl/rtl/include hdl/rtl/common/axi4s_skid_buffer.v

# Formal (BMC->k-induction "prove" + a reachability "cover" task)
cd hdl/formal && sby -f axi4s_skid_buffer.sby

# cocotb simulation via Verilator
cd hdl/sim/cocotb && ../.venv/bin/python -m pytest test_runner.py -v
```

See `hdl/docs/formal_conventions.md` for the formal harness pattern
(SVA-lite, `bind`-based white-box checks, small-parameterization) used
above and expected for every subsequent block.

## CI wiring

Tracked as a Phase 9.0 follow-up once more than one block exists (a
single-block CMake target would be premature abstraction); the commands
above are the ones CI will wrap. `ATSC3_ENABLE_HDL_STUBS=ON` gates the
existing `hdl/CMakeLists.txt` Verilator discovery; formal and cocotb
stages are invoked directly (as above) rather than through CMake, since
neither SymbiYosys nor cocotb's Python runner benefit from CMake's
C/C++-oriented build-graph model.
