# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Alfred Wanga
#
"""cocotb testbench for hdl/rtl/channel/equalizer.v

Bit-exact comparison against the real golden model
(lib/channel/equalizer.cc's Equalizer::equalize_sample(),
ATSC3_FIXED_POINT=ON) via hdl/sim/golden/equalizer_gen. The Q30 floor and
noise registers are the integers the model derives from the float
configuration (the generator's K line), so the RTL is programmed with
exactly what software would write.

Covers ZF and MMSE; every combination of the Q1.15 corner values on all
four rails (|H|^2 reaches 2^31); the deep-fade floor from both the
configured value and the structural 2^15 on either side of each threshold;
quotients that clamp at +/-32767; truncation toward zero for both signs;
random full-range and small-magnitude carriers; a large noise term;
independent input gaps on the y and h streams and output backpressure with
the AXI4-S hold rule checked; TLAST and the per-symbol zeroed-carrier
count; a TLAST mismatch between the streams setting framing_error; an
out-of-range configuration clamping with cfg_invalid; and a reset in the
middle of a division.

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
GEN = REPO_ROOT / "build-fxp" / "hdl" / "sim" / "golden" / "equalizer_gen"


def golden(mode, min_mag, noise, carriers):
    """carriers: list of (y_re, y_im, h_re, h_im). Returns ((min_mag_sq_q30,
    noise_q30), [(re, im)])."""
    if not GEN.exists():
        raise FileNotFoundError(
            f"{GEN} not found -- build it first: cmake --build build-fxp "
            "--target equalizer_gen (requires -DATSC3_FIXED_POINT=ON "
            "-DATSC3_ENABLE_HDL_STUBS=ON)"
        )
    lines = [f"C {mode} {min_mag} {noise}"]
    lines += ["E %d %d %d %d" % c for c in carriers]
    out = subprocess.run(
        [str(GEN)],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()
    k = out[0].split()
    assert k[0] == "K"
    res = [tuple(int(v) for v in ln.split()) for ln in out[1:]]
    assert len(res) == len(carriers)
    return (int(k[1]), int(k[2])), res


def s16(v):
    v = int(v) & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def pack(re, im):
    return ((re & 0xFFFF) << 16) | (im & 0xFFFF)


async def reset_dut(dut, mode, min_mag_q30, noise_q30):
    dut.rst.value = 1
    dut.cfg_mode.value = mode
    dut.cfg_min_mag_sq_q30.value = min_mag_q30
    dut.cfg_noise_q30.value = noise_q30
    for sig in ("y", "h"):
        getattr(dut, f"{sig}_axis_tvalid").value = 0
        getattr(dut, f"{sig}_axis_tdata").value = 0
        getattr(dut, f"{sig}_axis_tlast").value = 0
    dut.m_axis_tready.value = 1
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst.value = 0
    await RisingEdge(dut.clk)


async def drive(dut, sig, words, lasts, rnd, gaps):
    valid = getattr(dut, f"{sig}_axis_tvalid")
    data = getattr(dut, f"{sig}_axis_tdata")
    last = getattr(dut, f"{sig}_axis_tlast")
    ready = getattr(dut, f"{sig}_axis_tready")
    for w, lst in zip(words, lasts):
        if gaps and rnd.random() < 0.2:
            valid.value = 0
            for _ in range(rnd.randint(1, 5)):
                await RisingEdge(dut.clk)
        valid.value = 1
        data.value = w
        last.value = int(lst)
        while True:
            await RisingEdge(dut.clk)
            if ready.value == 1:
                break
    valid.value = 0


async def run(
    dut, mode, min_mag, noise, symbols, rnd, stalls=False, y_last_override=None
):
    """symbols: list of carrier lists. Checks every output, TLAST and the
    zeroed count at each symbol's last beat."""
    flat = [c for sym in symbols for c in sym]
    (min_q, noise_q), expected = golden(mode, min_mag, noise, flat)
    await reset_dut(dut, mode, min_q, noise_q)
    assert dut.cfg_invalid.value == 0 and dut.framing_error.value == 0
    lasts = []
    for sym in symbols:
        lasts += [i == len(sym) - 1 for i in range(len(sym))]
    y_lasts = lasts if y_last_override is None else y_last_override
    ydrv = cocotb.start_soon(
        drive(dut, "y", [pack(c[0], c[1]) for c in flat], y_lasts, rnd, stalls)
    )
    hdrv = cocotb.start_soon(
        drive(dut, "h", [pack(c[2], c[3]) for c in flat], lasts, rnd, stalls)
    )

    async def backpressure():
        while True:
            dut.m_axis_tready.value = int(rnd.random() < 0.5)
            await RisingEdge(dut.clk)

    bp = cocotb.start_soon(backpressure()) if stalls else None
    idx = 0
    held = None
    zeros = 0
    while idx < len(flat):
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value != 1:
            assert held is None, f"carrier {idx}: TVALID dropped while stalled"
            continue
        word = (int(dut.m_axis_tdata.value), int(dut.m_axis_tlast.value))
        if held is not None:
            assert word == held, f"carrier {idx}: output changed while stalled"
        if dut.m_axis_tready.value != 1:
            held = word
            continue
        held = None
        got = (s16(word[0] >> 16), s16(word[0]))
        assert (
            got == expected[idx]
        ), f"carrier {idx} {flat[idx]}: expected {expected[idx]}, got {got}"
        assert word[1] == int(lasts[idx]), f"carrier {idx}: TLAST"
        zeros = (0 if idx == 0 or lasts[idx - 1] else zeros) + (got == (0, 0))
        if lasts[idx]:
            assert int(dut.zeroed_carriers.value) == zeros, "zeroed_carriers"
        idx += 1
    await ydrv
    await hdrv
    if bp is not None:
        bp.kill()
        dut.m_axis_tready.value = 1
    for _ in range(5):
        await RisingEdge(dut.clk)
    assert dut.m_axis_tvalid.value == 0, "extra output after the last carrier"
    return expected


def clock(dut):
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())


def chunks(carriers, n):
    return [carriers[i : i + n] for i in range(0, len(carriers), n)]


@cocotb.test(timeout_time=600, timeout_unit="ms")
async def corners_zf_and_mmse(dut):
    """All corner-value combinations, both modes."""
    clock(dut)
    rnd = random.Random(0xF1)
    corners = [-32768, -32767, -1, 0, 1, 32767]
    carriers = list(itertools.product(corners, repeat=4))
    # y larger than h in magnitude: the quotient exceeds 1.0 and clamps
    carriers += [(32767, 0, 300, 0), (-32768, 32767, 0, -400), (20000, 20000, 250, 250)]
    for mode in (0, 1):
        exp = await run(dut, mode, "0.01", "0.01", chunks(carriers, 300), rnd)
        assert any(e == (0, 0) for e in exp)
        assert any(abs(e[0]) == 32767 for e in exp), "no clamp case"


@cocotb.test(timeout_time=600, timeout_unit="ms")
async def thresholds(dut):
    """Carriers whose |H|^2 sits just below / at / above the configured
    floor and the structural 2^15, and the MMSE denominator around 2^15."""
    clock(dut)
    rnd = random.Random(0xF2)
    for mode, min_mag, noise in [
        (0, "0.01", "0.01"),
        (0, "0.0", "0.0"),
        (0, "0.3", "0.01"),
        (0, "0.9", "0.5"),
        (1, "0.01", "0.0"),
        (1, "0.5", "0.00001"),
        (1, "0.01", "1.5"),
    ]:
        carriers = []
        for mag in [
            1,
            2,
            100,
            180,
            181,
            182,
            183,
            184,
            330,
            331,
            332,
            9830,
            9831,
            9832,
            29491,
            29492,
            29493,
        ]:
            for _ in range(6):
                hr = rnd.randint(-mag, mag)
                hi = rnd.choice([-1, 1]) * int((max(mag * mag - hr * hr, 0)) ** 0.5)
                carriers.append(
                    (rnd.randint(-32768, 32767), rnd.randint(-32768, 32767), hr, hi)
                )
        await run(dut, mode, min_mag, noise, chunks(carriers, 17), rnd)


@cocotb.test(timeout_time=900, timeout_unit="ms")
async def random_with_stalls(dut):
    """Random carriers of mixed scale, with independent y/h gaps and output
    backpressure, in several symbols."""
    clock(dut)
    rnd = random.Random(0xF3)
    for mode, min_mag, noise in [(0, "0.01", "0.01"), (1, "0.02", "0.05")]:
        carriers = []
        for _ in range(500):
            sy = rnd.choice([32768, 4096, 256])
            sh = rnd.choice([32768, 4096, 256, 40])
            carriers.append(
                (
                    max(-32768, min(32767, rnd.randint(-sy, sy))),
                    max(-32768, min(32767, rnd.randint(-sy, sy))),
                    max(-32768, min(32767, rnd.randint(-sh, sh))),
                    max(-32768, min(32767, rnd.randint(-sh, sh))),
                )
            )
        await run(dut, mode, min_mag, noise, chunks(carriers, 37), rnd, stalls=True)


@cocotb.test(timeout_time=100, timeout_unit="ms")
async def tlast_mismatch_sets_framing_error(dut):
    """y's TLAST disagreeing with h's sets framing_error; the output TLAST
    follows h."""
    clock(dut)
    rnd = random.Random(0xF4)
    carriers = [(1000, 500, 20000, -3000)] * 4
    try:
        await run(
            dut,
            0,
            "0.01",
            "0.01",
            [carriers],
            rnd,
            y_last_override=[False, True, False, False],
        )
    except AssertionError:
        raise
    assert dut.framing_error.value == 1


@cocotb.test(timeout_time=100, timeout_unit="ms")
async def invalid_config_clamps(dut):
    """Bit 31 set in a Q30 register: clamped to 2^31 - 1 with cfg_invalid.
    The clamped floor zeroes everything in ZF mode."""
    clock(dut)
    await reset_dut(dut, 0, 0x80000001, 0)
    assert dut.cfg_invalid.value == 1
    dut.y_axis_tvalid.value = 1
    dut.h_axis_tvalid.value = 1
    dut.y_axis_tdata.value = pack(1000, 1000)
    dut.h_axis_tdata.value = pack(32767, 0)
    dut.y_axis_tlast.value = 1
    dut.h_axis_tlast.value = 1
    for _ in range(100):
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value == 1:
            break
    assert dut.m_axis_tvalid.value == 1 and int(dut.m_axis_tdata.value) == 0
    await reset_dut(dut, 0, 0, 0x7FFFFFFF)
    assert dut.cfg_invalid.value == 0


@cocotb.test(timeout_time=100, timeout_unit="ms")
async def reset_mid_division(dut):
    """A reset abandons a carrier in flight; the next symbol is correct."""
    clock(dut)
    await reset_dut(dut, 0, 107374, 10737418)
    dut.y_axis_tvalid.value = 1
    dut.h_axis_tvalid.value = 1
    dut.y_axis_tdata.value = pack(12000, -3000)
    dut.h_axis_tdata.value = pack(30000, 100)
    dut.y_axis_tlast.value = 0
    dut.h_axis_tlast.value = 0
    await RisingEdge(dut.clk)
    await RisingEdge(dut.clk)
    dut.y_axis_tvalid.value = 0
    dut.h_axis_tvalid.value = 0
    for _ in range(8):
        await RisingEdge(dut.clk)
    dut.rst.value = 1
    await RisingEdge(dut.clk)
    dut.rst.value = 0
    for _ in range(80):
        await RisingEdge(dut.clk)
        assert dut.m_axis_tvalid.value == 0, "output after reset"
    await run(
        dut,
        0,
        "0.01",
        "0.01",
        [[(12000, -3000, 30000, 100), (1, 1, 32767, 0)]],
        random.Random(2),
    )
