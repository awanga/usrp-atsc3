# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Alfred Wanga
#
"""cocotb testbench for hdl/rtl/channel/complex_divider.v

Bit-exact comparison against the real golden model
(lib/channel/channel_estimator.cc's safe_complex_divide(),
ATSC3_FIXED_POINT=ON) via hdl/sim/golden/channel_estimator_gen's D lines.

Covers the Q1.15 corners (+/-32768 and +/-32767 on every operand rail,
where |b|^2 reaches 2^31 and the numerators 2^31), the |b|^2 < 2^15 guard
on both sides of the threshold, a zero divisor, results that clamp at
+/-32767, exact quotients and truncation toward zero for both signs,
random full-range operands, TLAST carried through, output backpressure
with the AXI4-S hold rule checked, and a reset in the middle of a
division.

Run via test_runner.py (pytest), not directly.
"""

import itertools
import random
import subprocess
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge

REPO_ROOT = Path(__file__).resolve().parents[3]
GEN = REPO_ROOT / "build-fxp" / "hdl" / "sim" / "golden" / "channel_estimator_gen"


def golden_divide(cases):
    if not GEN.exists():
        raise FileNotFoundError(
            f"{GEN} not found -- build it first: cmake --build build-fxp "
            "--target channel_estimator_gen (requires -DATSC3_FIXED_POINT=ON "
            "-DATSC3_ENABLE_HDL_STUBS=ON)"
        )
    text = "".join("D %d %d %d %d\n" % c for c in cases)
    out = subprocess.run(
        [str(GEN)], input=text, capture_output=True, text=True, check=True
    ).stdout
    res = [tuple(int(v) for v in line.split()) for line in out.splitlines()]
    assert len(res) == len(cases)
    return res


def u16(v):
    return v & 0xFFFF


def s16(v):
    v = int(v)
    return v - 0x10000 if v & 0x8000 else v


def pack_in(c):
    return (u16(c[0]) << 48) | (u16(c[1]) << 32) | (u16(c[2]) << 16) | u16(c[3])


async def reset_dut(dut):
    dut.rst.value = 1
    dut.s_axis_tvalid.value = 0
    dut.s_axis_tdata.value = 0
    dut.s_axis_tlast.value = 0
    dut.m_axis_tready.value = 1
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst.value = 0
    await RisingEdge(dut.clk)


async def run_cases(dut, cases, rnd, stalls=False):
    expected = golden_divide(cases)
    await reset_dut(dut)
    drive_tlast = [rnd.random() < 0.5 for _ in cases]

    async def driver():
        for c, last in zip(cases, drive_tlast):
            dut.s_axis_tvalid.value = 1
            dut.s_axis_tdata.value = pack_in(c)
            dut.s_axis_tlast.value = int(last)
            while True:
                await RisingEdge(dut.clk)
                if dut.s_axis_tready.value == 1:
                    break
            dut.s_axis_tvalid.value = 0
            if stalls:
                for _ in range(rnd.randint(0, 3)):
                    await RisingEdge(dut.clk)

    async def backpressure():
        while True:
            dut.m_axis_tready.value = int(rnd.random() < 0.4)
            await RisingEdge(dut.clk)

    if stalls:
        cocotb.start_soon(backpressure())
    drv = cocotb.start_soon(driver())
    idx = 0
    held = None
    while idx < len(cases):
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value != 1:
            assert held is None, f"case {idx}: TVALID dropped while stalled"
            continue
        word = (int(dut.m_axis_tdata.value), int(dut.m_axis_tlast.value))
        if held is not None:
            assert word == held, f"case {idx}: output changed while stalled"
        if dut.m_axis_tready.value != 1:
            held = word
            continue
        held = None
        got = (s16(word[0] >> 16), s16(word[0] & 0xFFFF))
        assert (
            got == expected[idx]
        ), f"case {idx} {cases[idx]}: expected {expected[idx]}, got {got}"
        assert word[1] == int(
            drive_tlast[idx]
        ), f"case {idx}: TLAST not carried through"
        idx += 1
    await drv
    for _ in range(100):
        await RisingEdge(dut.clk)
        assert dut.m_axis_tvalid.value == 0, "extra result after the last expected one"
    return expected


@cocotb.test(timeout_time=100, timeout_unit="ms")
async def corners_and_guard(dut):
    """Every combination of corner values on all four rails, plus the
    |b|^2 guard threshold and a zero divisor."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    corners = [-32768, -32767, -1, 0, 1, 32767]
    cases = list(itertools.product(corners, repeat=4))
    # |b|^2 = 32767 (just below the guard) and 32768 (just at it)
    cases += [(16384, 8192, 181, 2), (16384, 8192, 181, 3), (100, 50, 128, 128)]
    cases += [(100, 50, 0, 0), (-32768, -32768, 0, 0)]
    rnd = random.Random(0xD1)
    expected = await run_cases(dut, cases, rnd)
    assert any(e == (0, 0) for e in expected)
    assert any(abs(e[0]) == 32767 for e in expected), "no clamp case"


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def truncation_and_exact(dut):
    """Exact quotients and quotients with a remainder, both signs, so
    truncation toward zero (not floor) is what is compared."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xD2)
    cases = []
    for _ in range(300):
        x = rnd.choice([32767, 16384, 20000, 12345, 255, 32])
        y = 0 if rnd.random() < 0.6 else rnd.choice([0, 7, 1000])
        a = (rnd.randint(-32767, 32767), rnd.randint(-32767, 32767))
        sx = rnd.choice([-1, 1])
        cases.append((a[0], a[1], sx * x, y * rnd.choice([-1, 1])))
    await run_cases(dut, cases, rnd)


@cocotb.test(timeout_time=300, timeout_unit="ms")
async def random_with_stalls(dut):
    """Random full-range operands (small divisors included), with input
    gaps and output backpressure."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xD3)
    cases = []
    for _ in range(1500):
        scale = rnd.choice([32768, 4096, 256, 16])
        cases.append(
            (
                rnd.randint(-32768, 32767),
                rnd.randint(-32768, 32767),
                max(-32768, min(32767, rnd.randint(-scale, scale))),
                max(-32768, min(32767, rnd.randint(-scale, scale))),
            )
        )
    await run_cases(dut, cases, rnd, stalls=True)


@cocotb.test(timeout_time=100, timeout_unit="ms")
async def reset_mid_division(dut):
    """A reset abandons a division in flight; the next one is correct."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    await reset_dut(dut)
    dut.s_axis_tvalid.value = 1
    dut.s_axis_tdata.value = pack_in((12000, -3000, 30000, 100))
    await RisingEdge(dut.clk)
    while dut.s_axis_tready.value != 1:
        await RisingEdge(dut.clk)
    await RisingEdge(dut.clk)
    dut.s_axis_tvalid.value = 0
    for _ in range(10):
        await RisingEdge(dut.clk)
    dut.rst.value = 1
    await RisingEdge(dut.clk)
    dut.rst.value = 0
    for _ in range(80):
        await RisingEdge(dut.clk)
        assert dut.m_axis_tvalid.value == 0, "result after reset"
    await run_cases(
        dut, [(12000, -3000, 30000, 100), (1, 1, 32767, 0)], random.Random(1)
    )
