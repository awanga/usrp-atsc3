# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Alfred Wanga
#
"""cocotb testbench for hdl/rtl/channel/channel_estimator.v

Bit-exact comparison against the real golden model
(lib/channel/channel_estimator.cc's ChannelEstimator::estimate(),
ATSC3_FIXED_POINT=ON) via hdl/sim/golden/channel_estimator_gen: every
output carrier of every symbol, and TLAST on the last carrier.

Pilot sets: the real pilot_extractor layouts (8K, several patterns, random
received values, +/-32767 BPSK references) and random sparse sets with
edge pilots, random references and extreme received values at 8K, 16K and
32K, with and without carrier reduction (so first_active moves). Temporal
smoothing: alpha 0, 1, a mid value and 32768 over several symbols (the
first symbol must pass through, the rest mix with the stored one), and
enable low (nothing stored). Output backpressure and pilot-input gaps with
the AXI4-S hold rule checked. Invalid configuration falling back with its
flag set. The sticky order and span flags on out-of-order, out-of-window
and off-edge pilots (output framing must still be num_active beats). A
reset mid-symbol.

Run via test_runner.py (pytest), not directly.
"""

import random
import subprocess
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge

REPO_ROOT = Path(__file__).resolve().parents[3]
GOLDEN_DIR = REPO_ROOT / "build-fxp" / "hdl" / "sim" / "golden"
EST_GEN = GOLDEN_DIR / "channel_estimator_gen"
PILOT_GEN = GOLDEN_DIR / "pilot_extractor_gen"

NOC_MAX = {8192: 6913, 16384: 13825, 32768: 27649}
CUNIT = {8192: 96, 16384: 192, 32768: 384}


def s16(v):
    v = int(v) & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def first_active(fft, noc):
    return (fft - noc) // 2


def pack_pilot(carrier, ref, rx):
    return (
        (carrier & 0xFFFF)
        | ((ref[1] & 0xFFFF) << 16)
        | ((ref[0] & 0xFFFF) << 32)
        | ((rx[1] & 0xFFFF) << 48)
        | ((rx[0] & 0xFFFF) << 64)
    )


def run_golden(fft, noc, avg, alpha, symbols):
    """symbols: list of pilot lists [(carrier, ref, rx)]. Returns a list of
    per-symbol carrier lists [(re, im)]."""
    for tool in (EST_GEN,):
        if not tool.exists():
            raise FileNotFoundError(
                f"{tool} not found -- build it first: cmake --build build-fxp "
                "--target channel_estimator_gen (requires -DATSC3_FIXED_POINT=ON "
                "-DATSC3_ENABLE_HDL_STUBS=ON)"
            )
    lines = [f"C {fft} {noc} {int(avg)} {alpha}"]
    for i, pilots in enumerate(symbols):
        lines.append(f"S {i} {len(pilots)}")
        for c, ref, rx in pilots:
            lines.append(f"{c} {ref[0]} {ref[1]} {rx[0]} {rx[1]}")
    out = subprocess.run(
        [str(EST_GEN)],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()
    res, pos = [], 0
    while pos < len(out):
        n = int(out[pos].split()[2])
        res.append(
            [tuple(int(v) for v in ln.split()) for ln in out[pos + 1 : pos + 1 + n]]
        )
        pos += 1 + n
    assert len(res) == len(symbols)
    return res


def extractor_pilots(rnd, fft, pattern, sym_idx, noc=0):
    """Real pilot_extractor records for one random FFT symbol."""
    lines = [f"C {fft} {pattern} {noc}", f"S {sym_idx}"]
    lines += [
        f"{rnd.randint(-32768, 32767)} {rnd.randint(-32768, 32767)}" for _ in range(fft)
    ]
    out = subprocess.run(
        [str(PILOT_GEN)],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()[1:]
    pilots = []
    for ln in out:
        c, _t, rre, rim, xre, xim = (int(v) for v in ln.split())
        pilots.append((c, (rre, rim), (xre, xim)))
    return pilots


def random_pilots(rnd, fft, noc, count, ref_mode="bpsk"):
    first = first_active(fft, noc)
    inner = rnd.sample(range(1, noc - 1), count - 2)
    rel = sorted([0, noc - 1] + inner)
    pilots = []
    for r in rel:
        if ref_mode == "bpsk":
            ref = (rnd.choice([32767, -32767]), 0)
        else:
            sc = rnd.choice([32768, 2048, 300, 40])
            ref = (rnd.randint(-sc, sc - 1), rnd.randint(-sc, sc - 1))
        mode = rnd.random()
        if mode < 0.15:
            rx = (rnd.choice([-32768, 32767]), rnd.choice([-32768, 32767]))
        else:
            sc = rnd.choice([32768, 8192, 512])
            rx = (rnd.randint(-sc, sc - 1), rnd.randint(-sc, sc - 1))
        pilots.append((first + r, ref, rx))
    return pilots


async def reset_dut(dut, fft, noc, avg, alpha, flags=(0, 0, 0)):
    dut.rst.value = 1
    dut.cfg_fft_size.value = fft
    dut.cfg_num_active_carriers.value = noc
    dut.cfg_avg_enable.value = int(avg)
    dut.cfg_avg_alpha_q15.value = alpha
    dut.s_axis_tvalid.value = 0
    dut.s_axis_tdata.value = 0
    dut.s_axis_tlast.value = 0
    dut.m_axis_tready.value = 1
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst.value = 0
    await RisingEdge(dut.clk)
    got = (
        int(dut.cfg_fft_size_invalid.value),
        int(dut.cfg_num_active_invalid.value),
        int(dut.cfg_alpha_invalid.value),
    )
    assert got == flags, f"config flags {got}, expected {flags}"
    assert dut.span_error.value == 0 and dut.order_error.value == 0


async def drive(dut, symbols, rnd, gaps):
    for pilots in symbols:
        for i, (c, ref, rx) in enumerate(pilots):
            if gaps and rnd.random() < 0.1:
                dut.s_axis_tvalid.value = 0
                for _ in range(rnd.randint(1, 4)):
                    await RisingEdge(dut.clk)
            dut.s_axis_tvalid.value = 1
            dut.s_axis_tdata.value = pack_pilot(c, ref, rx)
            dut.s_axis_tlast.value = int(i == len(pilots) - 1)
            while True:
                await RisingEdge(dut.clk)
                if dut.s_axis_tready.value == 1:
                    break
    dut.s_axis_tvalid.value = 0


async def backpressure(dut, rnd):
    while True:
        dut.m_axis_tready.value = int(rnd.random() < 0.6)
        await RisingEdge(dut.clk)


async def collect(dut, expected_symbols):
    """expected_symbols: list of carrier lists or ints (beat count only)."""
    held = None
    for s, exp in enumerate(expected_symbols):
        n = exp if isinstance(exp, int) else len(exp)
        got = []
        while len(got) < n:
            await RisingEdge(dut.clk)
            if dut.m_axis_tvalid.value != 1:
                assert held is None, f"symbol {s}: TVALID dropped while stalled"
                continue
            word = (int(dut.m_axis_tdata.value), int(dut.m_axis_tlast.value))
            if held is not None:
                assert word == held, f"symbol {s}: data changed while stalled"
            if dut.m_axis_tready.value != 1:
                held = word
                continue
            held = None
            k = len(got)
            assert word[1] == int(k == n - 1), f"symbol {s} carrier {k}: TLAST wrong"
            got.append((s16(word[0] >> 16), s16(word[0])))
        if not isinstance(exp, int):
            for k, (g, e) in enumerate(zip(got, exp)):
                assert g == e, f"symbol {s} carrier {k}: expected {e}, got {g}"


async def run_stream(
    dut, fft, noc, avg, alpha, symbols, rnd, stalls=False, cfg=None, flags=(0, 0, 0)
):
    expected = run_golden(fft, noc, avg, alpha, symbols)
    await reset_dut(dut, *(cfg or (fft, noc, avg, alpha)), flags=flags)
    bp = cocotb.start_soon(backpressure(dut, rnd)) if stalls else None
    drv = cocotb.start_soon(drive(dut, symbols, rnd, stalls))
    await collect(dut, expected)
    await drv
    if bp is not None:
        bp.kill()
        dut.m_axis_tready.value = 1
    for _ in range(5):
        await RisingEdge(dut.clk)
    assert dut.m_axis_tvalid.value == 0, "extra beat after the last symbol"
    assert dut.span_error.value == 0 and dut.order_error.value == 0
    return expected


def clock(dut):
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())


@cocotb.test(timeout_time=3000, timeout_unit="ms")
async def extractor_layouts_8k(dut):
    """Real pilot layouts: SP6_2 (4), SP12_4 (9), SP3_2 (0) and SP32_2
    (14), one symbol each (the first-symbol offset differs per pattern)."""
    clock(dut)
    rnd = random.Random(0xE1)
    for pattern in (4, 9, 0, 14):
        pilots = extractor_pilots(rnd, 8192, pattern, 1)
        await run_stream(dut, 8192, 0, False, 0, [pilots], rnd)


@cocotb.test(timeout_time=3000, timeout_unit="ms")
async def sparse_sets_all_sizes(dut):
    """Random sparse sets (edges, random references incl. tiny ones that
    hit the divider guard, extreme received values) at each FFT size,
    default and carrier-reduced windows."""
    clock(dut)
    rnd = random.Random(0xE2)
    for fft in (8192, 16384, 32768):
        for red in (0, 2):
            noc = NOC_MAX[fft] - red * CUNIT[fft]
            cfg_noc = 0 if red == 0 else noc
            symbols = [
                random_pilots(rnd, fft, noc, rnd.randint(2, 60), "bpsk"),
                random_pilots(rnd, fft, noc, rnd.randint(2, 60), "wide"),
            ]
            await run_stream(dut, fft, cfg_noc, False, 0, symbols, rnd)


@cocotb.test(timeout_time=3000, timeout_unit="ms")
async def adjacent_and_dense_pilots(dut):
    """Spacing 1 (D = 1), a long segment, and a full-density run."""
    clock(dut)
    rnd = random.Random(0xE3)
    first = first_active(8192, 6913)
    last = first + 6912

    def mk(rel):
        return [
            (
                first + r,
                (rnd.choice([32767, -32767]), 0),
                (rnd.randint(-32768, 32767), rnd.randint(-32768, 32767)),
            )
            for r in rel
        ]

    symbols = [
        mk([0, 1, 2, 3, 4, 6912]),
        mk([0, 6912]),
        mk(list(range(0, 300)) + [6912]),
    ]
    assert symbols[0][-1][0] == last
    await run_stream(dut, 8192, 0, False, 0, symbols, rnd)


@cocotb.test(timeout_time=3000, timeout_unit="ms")
async def averaging(dut):
    """Smoothing across symbols for several alphas; the first symbol passes
    through, and alpha = 32768 reproduces the input."""
    clock(dut)
    rnd = random.Random(0xE4)
    for alpha in (0, 1, 3276, 16384, 32767, 32768):
        symbols = [random_pilots(rnd, 8192, 6913, 40, "bpsk") for _ in range(4)]
        await run_stream(dut, 8192, 0, True, alpha, symbols, rnd)
    symbols = [random_pilots(rnd, 8192, 6913, 40, "bpsk") for _ in range(3)]
    await run_stream(dut, 8192, 0, False, 3276, symbols, rnd)


@cocotb.test(timeout_time=3000, timeout_unit="ms")
async def stalls_and_gaps(dut):
    """Input gaps and output backpressure, with smoothing on."""
    clock(dut)
    rnd = random.Random(0xE5)
    symbols = [random_pilots(rnd, 8192, 6913, 30, "wide") for _ in range(3)]
    await run_stream(dut, 8192, 0, True, 5000, symbols, rnd, stalls=True)


@cocotb.test(timeout_time=3000, timeout_unit="ms")
async def invalid_config_falls_back(dut):
    """Bad FFT size -> 8K; bad active count -> NoCmax; alpha > 32768 ->
    32768. Each sets its flag, and the output equals the default run."""
    clock(dut)
    rnd = random.Random(0xE6)
    symbols = [random_pilots(rnd, 8192, 6913, 20, "bpsk") for _ in range(2)]
    await run_stream(
        dut,
        8192,
        0,
        True,
        32768,
        symbols,
        rnd,
        cfg=(12345, 0, True, 32768),
        flags=(1, 0, 0),
    )
    await run_stream(
        dut,
        8192,
        0,
        True,
        32768,
        symbols,
        rnd,
        cfg=(8192, 5000, True, 32768),
        flags=(0, 1, 0),
    )
    await run_stream(
        dut,
        8192,
        0,
        True,
        32768,
        symbols,
        rnd,
        cfg=(8192, 0, True, 40000),
        flags=(0, 0, 1),
    )


@cocotb.test(timeout_time=3000, timeout_unit="ms")
async def malformed_pilot_streams(dut):
    """Out-of-order, duplicate, out-of-window and below-window pilots are
    dropped with order_error; a first pilot off the edge sets span_error.
    Every symbol still has exactly num_active beats."""
    clock(dut)
    rnd = random.Random(0xE7)
    fft, noc = 8192, 6913
    first = first_active(fft, noc)

    def pil(rel):
        return (
            first + rel,
            (32767, 0),
            (rnd.randint(-9000, 9000), rnd.randint(-9000, 9000)),
        )

    cases = {
        "order": ([pil(0), pil(500), pil(400), pil(6912)], 1, 0),
        "dup": ([pil(0), pil(500), pil(500), pil(6912)], 1, 0),
        "beyond": ([pil(0), pil(500), pil(7000)], 1, 0),
        "below": ([(first - 1, (32767, 0), (5, 5)), pil(0), pil(6912)], 1, 0),
        "offedge": ([pil(10), pil(500), pil(6912)], 0, 1),
    }
    for name, (pilots, want_order, want_span) in cases.items():
        await reset_dut(dut, fft, 0, False, 0)
        drv = cocotb.start_soon(drive(dut, [pilots], rnd, False))
        await collect(dut, [noc])
        await drv
        for _ in range(5):
            await RisingEdge(dut.clk)
        assert int(dut.order_error.value) == want_order, f"{name}: order_error"
        assert int(dut.span_error.value) == want_span, f"{name}: span_error"
    # A flag is cleared by reset only
    await reset_dut(dut, fft, 0, False, 0)


@cocotb.test(timeout_time=3000, timeout_unit="ms")
async def reset_mid_symbol(dut):
    """A reset part-way through a symbol; the next symbol is clean and the
    stored smoothing state is forgotten."""
    clock(dut)
    rnd = random.Random(0xE8)
    fft, noc = 8192, 6913
    cut = random_pilots(rnd, fft, noc, 30, "bpsk")
    await reset_dut(dut, fft, 0, True, 8000)
    dut.m_axis_tready.value = 1
    drv = cocotb.start_soon(drive(dut, [cut], rnd, False))
    for _ in range(3000):
        await RisingEdge(dut.clk)
    drv.kill()
    symbols = [random_pilots(rnd, fft, noc, 30, "bpsk") for _ in range(2)]
    await run_stream(dut, fft, 0, True, 8000, symbols, rnd)
