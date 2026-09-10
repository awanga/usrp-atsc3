"""cocotb testbench for hdl/rtl/common/cordic.v

Bit-exact comparison against the real golden model (lib/dsp/cordic.h/.cc),
run through the hdl/sim/golden/cordic_gen CLI -- never a Python
reimplementation of the algorithm, so there is exactly one source of truth
for what "correct" means. Both this test and the golden CLI operate on the
same in-memory stimulus list (generated once, here), so neither side can
independently drift from the other's waveform.

Run directly: hdl/sim/.venv/bin/python -m pytest test_cordic.py
"""

import random
import subprocess
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge

REPO_ROOT = Path(__file__).resolve().parents[3]
CORDIC_GEN = REPO_ROOT / "build-fxp" / "hdl" / "sim" / "golden" / "cordic_gen"

CORDIC_MODE_ROTATE = 0
CORDIC_MODE_VECTOR = 1

MAX_WAIT_CYCLES = 64  # generous vs. the 14-iteration datapath's real latency


def _q15(v):
    """Wrap a Python int into the signed 16-bit range, matching Verilog's
    2's-complement truncation for the same bit pattern."""
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def generate_stimulus(rnd):
    """Returns a list of (mode, in_a, in_b) tuples. Boundary cases first
    (deterministic), then randomized coverage of the full Q1.15 range."""
    cases = []

    # Rotation mode boundaries: axis-aligned angles and the two asymmetric
    # extremes of the Q1.15 angle format.
    for theta in (0, 16384, -16384, 32767, -32768, 8192, -8192):
        cases.append((CORDIC_MODE_ROTATE, theta, 0))

    # Vector mode boundaries: the degenerate origin, each axis, and each
    # quadrant corner.
    for x, y in ((0, 0), (32767, 0), (-32768, 0), (0, 32767), (0, -32768),
                 (32767, 32767), (-32768, 32767), (32767, -32768), (-32768, -32768)):
        cases.append((CORDIC_MODE_VECTOR, x, y))

    for _ in range(60):
        cases.append((CORDIC_MODE_ROTATE, rnd.randint(-32768, 32767), 0))
    for _ in range(60):
        cases.append((CORDIC_MODE_VECTOR, rnd.randint(-32768, 32767), rnd.randint(-32768, 32767)))

    return cases


def run_golden(cases):
    """Feeds all cases to the golden CLI in one pass and returns a parallel
    list of (out_a, out_b) results."""
    if not CORDIC_GEN.exists():
        raise FileNotFoundError(
            f"{CORDIC_GEN} not found -- build it first: "
            "cmake --build build-fxp --target cordic_gen "
            "(requires -DATSC3_FIXED_POINT=ON -DATSC3_ENABLE_HDL_STUBS=ON)"
        )

    lines = []
    for mode, a, b in cases:
        if mode == CORDIC_MODE_ROTATE:
            lines.append(f"R {a}")
        else:
            lines.append(f"V {a} {b}")

    proc = subprocess.run(
        [str(CORDIC_GEN)],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        check=True,
    )
    results = []
    for line in proc.stdout.strip().splitlines():
        out_a, out_b = line.split()
        results.append((int(out_a), int(out_b)))
    assert len(results) == len(cases), (
        f"golden CLI returned {len(results)} results for {len(cases)} stimulus lines"
    )
    return results


async def reset_dut(dut):
    dut.rst.value = 1
    dut.start.value = 0
    dut.mode.value = 0
    dut.in_a.value = 0
    dut.in_b.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst.value = 0
    await RisingEdge(dut.clk)


@cocotb.test()
async def bit_exact_vs_golden_model(dut):
    rnd = random.Random(0xC0271C)
    cases = generate_stimulus(rnd)
    golden = run_golden(cases)

    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    await reset_dut(dut)

    for idx, ((mode, in_a, in_b), (exp_out_a, exp_out_b)) in enumerate(zip(cases, golden)):
        assert dut.busy.value == 0, f"case {idx}: DUT unexpectedly busy before start"

        dut.mode.value = mode
        dut.in_a.value = in_a & 0xFFFF
        dut.in_b.value = in_b & 0xFFFF
        dut.start.value = 1
        await RisingEdge(dut.clk)
        dut.start.value = 0

        for cycle in range(MAX_WAIT_CYCLES):
            await RisingEdge(dut.clk)
            if dut.done.value == 1:
                break
        else:
            raise TimeoutError(f"case {idx} (mode={mode}, in_a={in_a}, in_b={in_b}): "
                                f"done never asserted within {MAX_WAIT_CYCLES} cycles")

        got_out_a = _q15(int(dut.out_a.value))
        got_out_b = int(dut.out_b.value.signed_integer)

        assert got_out_a == exp_out_a, (
            f"case {idx} (mode={mode}, in_a={in_a}, in_b={in_b}): "
            f"out_a mismatch, expected {exp_out_a}, got {got_out_a}"
        )
        assert got_out_b == exp_out_b, (
            f"case {idx} (mode={mode}, in_a={in_a}, in_b={in_b}): "
            f"out_b mismatch, expected {exp_out_b}, got {got_out_b}"
        )
