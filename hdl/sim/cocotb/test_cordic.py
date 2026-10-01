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
from cocotb.triggers import ReadOnly, RisingEdge, Timer

REPO_ROOT = Path(__file__).resolve().parents[3]
CORDIC_GEN = REPO_ROOT / "build-fxp" / "hdl" / "sim" / "golden" / "cordic_gen"

CORDIC_MODE_ROTATE = 0
CORDIC_MODE_VECTOR = 1

CORDIC_ITERATIONS = 14  # lib/dsp/cordic.h kCordicIterations
LATENCY = CORDIC_ITERATIONS + 2  # cordic.v contract: start edge to done


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
    for x, y in (
        (0, 0),
        (32767, 0),
        (-32768, 0),
        (0, 32767),
        (0, -32768),
        (32767, 32767),
        (-32768, 32767),
        (32767, -32768),
        (-32768, -32768),
    ):
        cases.append((CORDIC_MODE_VECTOR, x, y))

    for _ in range(60):
        cases.append((CORDIC_MODE_ROTATE, rnd.randint(-32768, 32767), 0))
    for _ in range(60):
        cases.append(
            (CORDIC_MODE_VECTOR, rnd.randint(-32768, 32767), rnd.randint(-32768, 32767))
        )

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
    assert len(results) == len(
        cases
    ), f"golden CLI returned {len(results)} results for {len(cases)} stimulus lines"
    return results


async def edge(dut):
    """Next rising edge, with that edge's register updates settled."""
    await RisingEdge(dut.clk)
    await ReadOnly()


async def drive_point():
    """Leave the ReadOnly phase so inputs can be written for the next edge."""
    await Timer(1, "ns")


async def reset_dut(dut):
    dut.rst.value = 1
    dut.start.value = 0
    dut.mode.value = 0
    dut.in_a.value = 0
    dut.in_b.value = 0
    for _ in range(3):
        await edge(dut)
    await drive_point()
    dut.rst.value = 0
    await edge(dut)
    await drive_point()


@cocotb.test(timeout_time=10, timeout_unit="ms")
async def bit_exact_vs_golden_model(dut):
    """Every case bit-exact against the golden model, plus the handshake
    contract: done exactly LATENCY cycles after the start edge (2 for the
    vector (0, 0) bypass), busy high until then, outputs held until done,
    start ignored while busy."""
    rnd = random.Random(0xC0271C)
    cases = generate_stimulus(rnd)
    golden = run_golden(cases)

    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    await reset_dut(dut)

    checks = 0
    for idx, ((mode, in_a, in_b), (exp_out_a, exp_out_b)) in enumerate(
        zip(cases, golden)
    ):
        what = f"case {idx} (mode={mode}, in_a={in_a}, in_b={in_b})"
        latency = (
            2 if (mode == CORDIC_MODE_VECTOR and in_a == 0 and in_b == 0) else LATENCY
        )
        assert dut.busy.value == 0, f"{what}: DUT unexpectedly busy before start"
        held = (int(dut.out_a.value), int(dut.out_b.value))

        dut.mode.value = mode
        dut.in_a.value = in_a & 0xFFFF
        dut.in_b.value = in_b & 0xFFFF
        dut.start.value = 1
        await edge(dut)  # start sampled here
        await drive_point()
        dut.start.value = idx % 3 == 0  # a start while busy must be ignored
        dut.in_a.value = rnd.getrandbits(16)  # operands sampled at start only
        for cycle in range(1, latency):
            await edge(dut)
            assert dut.busy.value == 1, f"{what}: busy low {cycle} cycle(s) after start"
            assert (
                dut.done.value == 0
            ), f"{what}: done after {cycle} cycle(s), expected {latency}"
            assert (
                int(dut.out_a.value),
                int(dut.out_b.value),
            ) == held, f"{what}: outputs changed before done"
            await drive_point()
            dut.start.value = 0
        await edge(dut)
        assert dut.done.value == 1, f"{what}: no done {latency} cycles after start"
        assert dut.busy.value == 0

        got_out_a = _q15(int(dut.out_a.value))
        got_out_b = int(dut.out_b.value.signed_integer)
        assert (
            got_out_a == exp_out_a
        ), f"{what}: out_a expected {exp_out_a}, got {got_out_a}"
        assert (
            got_out_b == exp_out_b
        ), f"{what}: out_b expected {exp_out_b}, got {got_out_b}"
        checks += 1
        await drive_point()
    assert checks == len(cases) > 0
