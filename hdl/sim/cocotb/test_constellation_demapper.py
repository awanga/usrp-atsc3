# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Alfred Wanga
#
"""cocotb testbench for hdl/rtl/ofdm/constellation_demapper.v

Bit-exact comparison against the real golden model
(lib/ofdm/constellation_demapper.cc's demap_symbol(), ATSC3_FIXED_POINT=ON)
via hdl/sim/golden/demapper_gen. The Q16.16 LLR scale and the uniform
slicer step programmed into the RTL are the integers the model derives
(the generator's K line), as software would write them.

Covers every uniform order (QPSK .. 4096-QAM) and every non-uniform mode
(NUC_QPSK, 2D NUC 16/64/256, 1D NUC 1024/4096, the latter at several code
rates so every ROM region is read); cells at the Q1.15 corners, on and
next to decision boundaries (grid values and their neighbours), and random
full-range ones; scale and clip extremes (clip 1, 127, saturating scales);
TLAST carried to the last LLR beat of a TLAST cell; input gaps and
output backpressure with the AXI4-S hold rule checked; out-of-range
configuration falling back with cfg_invalid; a reset mid-cell.

Run via test_runner.py (pytest), not directly.
"""

import random
import subprocess
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge

REPO_ROOT = Path(__file__).resolve().parents[3]
GEN = REPO_ROOT / "build-fxp" / "hdl" / "sim" / "golden" / "demapper_gen"

UNIFORM = [0, 1, 2, 3, 4, 5]
NUC = [6, 7, 8, 9, 10, 11]
BPS = {0: 2, 1: 4, 2: 6, 3: 8, 4: 10, 5: 12, 6: 2, 7: 4, 8: 6, 9: 8, 10: 10, 11: 12}


def golden(mod, rate, noise, clip, cells):
    """Returns ((bps, scale_q16, norm_q15), [llr list per cell])."""
    if not GEN.exists():
        raise FileNotFoundError(
            f"{GEN} not found -- build it first: cmake --build build-fxp "
            "--target demapper_gen (requires -DATSC3_FIXED_POINT=ON "
            "-DATSC3_ENABLE_HDL_STUBS=ON)"
        )
    lines = [f"C {mod} {rate} {noise} {clip}"]
    lines += [f"X {re} {im}" for re, im in cells]
    out = subprocess.run(
        [str(GEN)],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()
    k = out[0].split()
    assert k[0] == "K"
    res = [[int(v) for v in ln.split()] for ln in out[1:]]
    assert len(res) == len(cells)
    return (int(k[1]), int(k[2]), int(k[3])), res


def s8(v):
    v = int(v) & 0xFF
    return v - 256 if v & 0x80 else v


def pack(re, im):
    return ((re & 0xFFFF) << 16) | (im & 0xFFFF)


async def reset_dut(dut, mod, rate, scale, norm, clip, invalid=0):
    dut.rst.value = 1
    dut.cfg_modulation.value = mod
    dut.cfg_code_rate.value = rate
    dut.cfg_llr_scale_q16.value = scale
    dut.cfg_norm_const_q15.value = norm
    dut.cfg_llr_clip.value = clip
    dut.s_axis_tvalid.value = 0
    dut.s_axis_tdata.value = 0
    dut.s_axis_tlast.value = 0
    dut.m_axis_tready.value = 1
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst.value = 0
    await RisingEdge(dut.clk)
    assert int(dut.cfg_invalid.value) == invalid, "cfg_invalid"


async def drive(dut, cells, lasts, rnd, gaps):
    for (re, im), last in zip(cells, lasts):
        if gaps and rnd.random() < 0.2:
            dut.s_axis_tvalid.value = 0
            for _ in range(rnd.randint(1, 4)):
                await RisingEdge(dut.clk)
        dut.s_axis_tvalid.value = 1
        dut.s_axis_tdata.value = pack(re, im)
        dut.s_axis_tlast.value = int(last)
        while True:
            await RisingEdge(dut.clk)
            if dut.s_axis_tready.value == 1:
                break
    dut.s_axis_tvalid.value = 0


def boundary_cells(rnd, mod, norm):
    """Coordinates exactly on and one below every cell boundary k * cw
    (cw = 2 * norm_const), both signs, on each axis, so the boundary
    distances (zero on the boundary itself) are exercised."""
    half = 1 << (BPS[mod] // 2 - 1)
    cw = 2 * norm
    cells = []
    for k in range(1, half + 1):
        for d in (-1, 0):
            for sign in (1, -1):
                v = max(-32768, min(32767, sign * (k * cw + d)))
                cells.append((v, rnd.randint(-32768, 32767)))
                cells.append((rnd.randint(-32768, 32767), v))
    return cells


def nuc_qpsk_sensitive(scale):
    """x values where the NUC_QPSK LLR (4 * x * c * scale, rounded >> 46)
    changes if the point value c = 23170 changes by one: the rounding
    boundaries, found with exact integer arithmetic."""
    c = 23170
    out = []
    for x in range(1, 32768):
        for sg in (1, -1):
            a = ((sg * 4 * x * c * scale) + (1 << 45)) >> 46
            b = ((sg * 4 * x * (c + 1) * scale) + (1 << 45)) >> 46
            if a != b and abs(a) < 120:
                out.append((x if sg > 0 else -x, 0))
                out.append((0, x if sg > 0 else -x))
    return out[:24]


async def run(dut, mod, rate, noise, clip, cells, rnd, stalls=False):
    (_, k_scale, k_norm), _ = golden(mod, rate, noise, clip, [])
    if mod in UNIFORM and k_norm > 0:
        cells = cells + boundary_cells(rnd, mod, k_norm)
    if mod == 6:
        cells = cells + nuc_qpsk_sensitive(k_scale)
    if mod in (0, 6):
        # Full-scale corners: QPSK's (-Im) + j(-Re) must saturate -32768.
        f = (-32768, 32767, 0)
        cells = cells + [(re, im) for re in f for im in f]
    (bps, scale, norm), expected = golden(mod, rate, noise, clip, cells)
    assert bps == BPS[mod]
    await reset_dut(dut, mod, rate, scale, norm, clip)
    lasts = [rnd.random() < 0.3 for _ in cells]
    drv = cocotb.start_soon(drive(dut, cells, lasts, rnd, stalls))

    async def backpressure():
        while True:
            dut.m_axis_tready.value = int(rnd.random() < 0.5)
            await RisingEdge(dut.clk)

    bp = cocotb.start_soon(backpressure()) if stalls else None
    flat = [(c, b, v) for c, cell in enumerate(expected) for b, v in enumerate(cell)]
    held = None
    n = 0
    while n < len(flat):
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value != 1:
            assert held is None, f"beat {n}: TVALID dropped while stalled"
            continue
        word = (s8(dut.m_axis_tdata.value), int(dut.m_axis_tlast.value))
        if held is not None:
            assert word == held, f"beat {n}: output changed while stalled"
        if dut.m_axis_tready.value != 1:
            held = word
            continue
        held = None
        c, b, v = flat[n]
        assert word[0] == v, f"cell {c} {cells[c]} bit {b}: expected {v}, got {word[0]}"
        assert word[1] == int(lasts[c] and b == bps - 1), f"cell {c} bit {b}: TLAST"
        n += 1
    await drv
    if bp is not None:
        bp.kill()
        dut.m_axis_tready.value = 1
    for _ in range(5):
        await RisingEdge(dut.clk)
    assert dut.m_axis_tvalid.value == 0, "extra beat after the last cell"
    return expected


def clock(dut):
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())


def cells_for(rnd, n, mod):
    """Corners, points on and next to decision boundaries, random cells."""
    cells = [
        (-32768, -32768),
        (32767, 32767),
        (-32768, 32767),
        (0, 0),
        (1, -1),
        (-32767, 0),
    ]
    step = {0: 23170, 1: 10362, 2: 5057, 3: 2513, 4: 1255, 5: 627}.get(mod, 4000)
    for _ in range(n // 3):
        g = rnd.randint(-40, 40) * step // 2
        delta = rnd.choice([-1, 0, 1, 2])
        cells.append((max(-32768, min(32767, g + delta)), rnd.randint(-32768, 32767)))
        cells.append((rnd.randint(-32768, 32767), max(-32768, min(32767, g + delta))))
    while len(cells) < n:
        sc = rnd.choice([32768, 16384, 4096])
        cells.append((rnd.randint(-sc, sc - 1), rnd.randint(-sc, sc - 1)))
    return cells[:n]


@cocotb.test(timeout_time=2000, timeout_unit="ms")
async def uniform_modes(dut):
    """Every uniform order, two noise variances."""
    clock(dut)
    rnd = random.Random(0xA1)
    for mod in UNIFORM:
        for noise in ("0.1", "0.01"):
            await run(dut, mod, 0, noise, 127, cells_for(rnd, 36, mod), rnd)


@cocotb.test(timeout_time=3000, timeout_unit="ms")
async def nuc_modes(dut):
    """Every non-uniform mode; the table-backed ones at several code rates
    so each ROM region (and the first and last entries) is read."""
    clock(dut)
    rnd = random.Random(0xA2)
    for mod in NUC:
        rates = [0] if mod == 6 else [0, 5, 11]
        for rate in rates:
            await run(dut, mod, rate, "0.1", 127, cells_for(rnd, 18, mod), rnd)
    # every code rate once for the two smaller 2D tables
    for mod in (7, 8):
        for rate in range(12):
            await run(dut, mod, rate, "0.05", 127, cells_for(rnd, 9, mod), rnd)


@cocotb.test(timeout_time=2000, timeout_unit="ms")
async def clip_and_scale_extremes(dut):
    """Clip 1, 5 and 127, a scale large enough to saturate every LLR, and a
    small one that rounds most to zero."""
    clock(dut)
    rnd = random.Random(0xA3)
    for mod in (0, 2, 5, 6, 8, 10):
        for noise, clip in [("0.0001", 127), ("0.1", 1), ("0.02", 5), ("5.0", 127)]:
            await run(dut, mod, 3, noise, clip, cells_for(rnd, 15, mod), rnd)


@cocotb.test(timeout_time=3000, timeout_unit="ms")
async def stalls_and_gaps(dut):
    """Input gaps and output backpressure, TLAST on random cells."""
    clock(dut)
    rnd = random.Random(0xA4)
    for mod in (1, 4, 7, 11):
        await run(dut, mod, 2, "0.05", 127, cells_for(rnd, 20, mod), rnd, stalls=True)


@cocotb.test(timeout_time=100, timeout_unit="ms")
async def invalid_config_falls_back(dut):
    """Modulation 12 -> QPSK, code rate 13 -> 0, clip 0 or 200 -> 127, a
    scale with bit 31 set clamped; each sets cfg_invalid."""
    clock(dut)
    rnd = random.Random(0xA5)
    cells = cells_for(rnd, 6, 0)
    (_, scale, norm), exp = golden(0, 0, "0.1", 127, cells)
    for args in [
        (12, 0, scale, norm, 127),
        (0, 13, scale, norm, 127),
        (0, 0, scale, norm, 0),
        (0, 0, scale, norm, 200),
    ]:
        await reset_dut(dut, *args, invalid=1)
        drv = cocotb.start_soon(drive(dut, cells, [False] * len(cells), rnd, False))
        for c in range(len(cells)):
            for b in range(2):
                while True:
                    await RisingEdge(dut.clk)
                    if dut.m_axis_tvalid.value == 1 and dut.m_axis_tready.value == 1:
                        break
                assert (
                    s8(dut.m_axis_tdata.value) == exp[c][b]
                ), f"{args} cell {c} bit {b}"
        await drv
    # bit 31 set in the scale clamps to 2^31 - 1 (all LLRs saturate at +/-127)
    await reset_dut(dut, 0, 0, 0x80000001, norm, 127, invalid=1)
    drv = cocotb.start_soon(drive(dut, [(20000, -20000)], [True], rnd, False))
    got = []
    while len(got) < 2:
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value == 1 and dut.m_axis_tready.value == 1:
            got.append(s8(dut.m_axis_tdata.value))
    await drv
    assert got == [-127, 127], got


@cocotb.test(timeout_time=100, timeout_unit="ms")
async def reset_mid_cell(dut):
    """A reset abandons a cell in flight; the next cell is correct."""
    clock(dut)
    rnd = random.Random(0xA6)
    (_, scale, norm), _ = golden(5, 0, "0.1", 127, [(1, 1)])
    await reset_dut(dut, 5, 0, scale, norm, 127)
    dut.s_axis_tvalid.value = 1
    dut.s_axis_tdata.value = pack(12000, -3000)
    await RisingEdge(dut.clk)
    await RisingEdge(dut.clk)
    dut.s_axis_tvalid.value = 0
    for _ in range(40):
        await RisingEdge(dut.clk)
    dut.rst.value = 1
    await RisingEdge(dut.clk)
    dut.rst.value = 0
    for _ in range(200):
        await RisingEdge(dut.clk)
        assert dut.m_axis_tvalid.value == 0, "output after reset"
    await run(dut, 5, 0, "0.1", 127, [(12000, -3000), (-5000, 777)], rnd)
