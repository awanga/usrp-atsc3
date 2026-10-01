"""cocotb testbench for hdl/rtl/ofdm/cp_removal.v

Bit-exact comparison against the real golden model (lib/ofdm/cp_removal.cc,
ATSC3_FIXED_POINT=ON) via the hdl/sim/golden/cp_removal_gen CLI -- never a
Python reimplementation of compute_cp_length()'s arithmetic. The RTL
streams straight through (no internal buffering) while the C++ reference
buffers a whole CP+FFT symbol before its callback fires (see
cp_removal.v's header comment); both emit the identical value sequence
and framing, just at different latency, so the comparison is against that
value/TLAST sequence, not cycle-exact timing.

Covers all 11 CpFraction values at FFT_8K, FFT_16K and FFT_32K to
exercise the numerator*scale multiply, multi-symbol continuity,
randomized backpressure on both sides of the interface, out-of-range
config clamping and a reset mid-symbol. Every scenario also counts output
beats, so a beat leaking out of the next symbol's CP fails.

Run via test_runner.py (pytest), not directly.
"""

import random
import subprocess
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge

REPO_ROOT = Path(__file__).resolve().parents[3]
CP_REMOVAL_GEN = REPO_ROOT / "build-fxp" / "hdl" / "sim" / "golden" / "cp_removal_gen"

# lib/ofdm/cp_removal.h:CpFraction, all 11 defined values
CP_FRACTIONS = [192, 384, 512, 768, 1024, 1536, 2048, 2432, 3072, 3648, 4096]

FFT_8K = 8192
FFT_16K = 16384
FFT_32K = 32768


def _clamp16(v):
    return max(-32768, min(32767, int(v)))


def run_golden(fft_size, cp_fraction_numerator, samples):
    if not CP_REMOVAL_GEN.exists():
        raise FileNotFoundError(
            f"{CP_REMOVAL_GEN} not found -- build it first: "
            "cmake --build build-fxp --target cp_removal_gen "
            "(requires -DATSC3_FIXED_POINT=ON -DATSC3_ENABLE_HDL_STUBS=ON)"
        )
    lines = [f"C {fft_size} {cp_fraction_numerator}"] + [
        f"{re} {im}" for re, im in samples
    ]
    proc = subprocess.run(
        [str(CP_REMOVAL_GEN)],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        check=True,
    )
    symbols = []
    current = None
    for line in proc.stdout.splitlines():
        if line.startswith("S "):
            current = []
            symbols.append(current)
        else:
            re_s, im_s = line.split()
            current.append((int(re_s), int(im_s)))
    return symbols


def expected_stream(symbols):
    """Flatten golden symbols into (data, last) beats, TLAST on each
    symbol's final sample."""
    out = []
    for sym in symbols:
        for i, (re, im) in enumerate(sym):
            out.append(((re, im), i == len(sym) - 1))
    return out


async def reset_dut(dut, fft_size, cp_fraction_numerator):
    dut.rst.value = 1
    dut.cfg_fft_size.value = fft_size
    dut.cfg_cp_fraction_numerator.value = cp_fraction_numerator
    dut.s_axis_tvalid.value = 0
    dut.s_axis_tdata.value = 0
    dut.s_axis_tlast.value = 0
    dut.m_axis_tready.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst.value = 0
    await RisingEdge(dut.clk)


def pack(re, im):
    return ((re & 0xFFFF) << 16) | (im & 0xFFFF)


def unpack(word):
    re = (word >> 16) & 0xFFFF
    im = word & 0xFFFF
    if re & 0x8000:
        re -= 1 << 16
    if im & 0x8000:
        im -= 1 << 16
    return re, im


async def drive_input(dut, samples, rnd, gaps):
    for re, im in samples:
        if gaps:
            for _ in range(rnd.randint(0, 2)):
                dut.s_axis_tvalid.value = 0
                await RisingEdge(dut.clk)
        dut.s_axis_tvalid.value = 1
        dut.s_axis_tdata.value = pack(re, im)
        while True:
            await RisingEdge(dut.clk)
            if dut.s_axis_tready.value == 1:
                break
    dut.s_axis_tvalid.value = 0


async def drive_backpressure(dut, rnd, stop_event):
    while not stop_event.is_set():
        dut.m_axis_tready.value = rnd.randint(0, 1)
        await RisingEdge(dut.clk)


async def collect_output(dut, expected, done_event):
    idx = 0
    while idx < len(expected):
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value == 1 and dut.m_axis_tready.value == 1:
            got = unpack(int(dut.m_axis_tdata.value))
            got_last = int(dut.m_axis_tlast.value)
            exp_data, exp_last = expected[idx]
            assert (
                got == exp_data
            ), f"beat {idx}: tdata mismatch, expected {exp_data}, got {got}"
            assert got_last == int(
                exp_last
            ), f"beat {idx}: tlast mismatch, expected {int(exp_last)}, got {got_last}"
            idx += 1
    done_event.set()


async def count_output(dut, counter):
    while True:
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value == 1 and dut.m_axis_tready.value == 1:
            counter[0] += 1


async def run_scenario(
    dut,
    fft_size,
    cp_fraction_numerator,
    samples,
    rnd,
    gaps=False,
    backpressure=False,
    golden_cfg=None,
    expect_flags=(0, 0),
):
    """golden_cfg: the (fft_size, numerator) the golden model should use,
    when the RTL is given an out-of-range config it must clamp to that."""
    await reset_dut(dut, fft_size, cp_fraction_numerator)

    golden = run_golden(*(golden_cfg or (fft_size, cp_fraction_numerator)), samples)
    expected = expected_stream(golden)
    assert expected, "golden model emitted no symbols -- test input too short"

    done_event = cocotb.triggers.Event()
    bp_task = None
    if backpressure:
        bp_task = cocotb.start_soon(drive_backpressure(dut, rnd, done_event))
    else:
        dut.m_axis_tready.value = 1
    fired = [0]
    count_task = cocotb.start_soon(count_output(dut, fired))

    # samples may run longer than what's needed to satisfy `expected` (a
    # deliberate idle/next-CP tail in some scenarios), so collect_output
    # can return while drive_input is still mid-stream. Awaiting both
    # tasks to full completion here keeps a leftover coroutine from one
    # scenario from racing the next scenario's reset_dut()/stimulus.
    drive_task = cocotb.start_soon(drive_input(dut, samples, rnd, gaps))
    await collect_output(dut, expected, done_event)
    await drive_task
    if bp_task is not None:
        await bp_task
    for _ in range(4):
        await RisingEdge(dut.clk)
    count_task.kill()
    assert fired[0] == len(
        expected
    ), f"{fired[0]} output beats, expected {len(expected)} (extra beats from the CP tail?)"

    assert (
        int(dut.cfg_fft_size_invalid.value),
        int(dut.cfg_cp_numerator_clamped.value),
    ) == expect_flags


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def all_cp_fractions_8k(dut):
    """All 11 defined CpFraction values at FFT_8K, one full symbol each
    plus a short idle tail."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xCB0192)

    for numerator in CP_FRACTIONS:
        symbol_len = FFT_8K + numerator  # exact for FFT_8K (scale == 1)
        samples = [
            (rnd.randint(-32768, 32767), rnd.randint(-32768, 32767))
            for _ in range(symbol_len + 8)
        ]
        await run_scenario(dut, FFT_8K, numerator, samples, rnd)


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def fft_16k_scaling(dut):
    """FFT_16K: cp_length = numerator * (fft_size >> 13) = numerator * 2,
    exercising the multiply the FFT_8K case can't (scale == 1 there)."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xF16C)

    for numerator in (512, 2048):
        cp_length = numerator * 2
        symbol_len = FFT_16K + cp_length
        samples = [
            (rnd.randint(-32768, 32767), rnd.randint(-32768, 32767))
            for _ in range(symbol_len + 8)
        ]
        await run_scenario(dut, FFT_16K, numerator, samples, rnd)


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def multi_symbol_continuity(dut):
    """Three consecutive symbols back to back at the default CpFraction,
    checking the counter restarts cleanly at each TLAST."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xC0117)

    numerator = 1024
    symbol_len = FFT_8K + numerator
    samples = [
        (rnd.randint(-32768, 32767), rnd.randint(-32768, 32767))
        for _ in range(3 * symbol_len)
    ]
    await run_scenario(dut, FFT_8K, numerator, samples, rnd)


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def randomized_backpressure(dut):
    """Random input gaps and random output stalls across two symbols --
    the black-box counterpart to the formal no-drop/framing proofs."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xBACC9E)

    numerator = 512
    symbol_len = FFT_8K + numerator
    samples = [
        (rnd.randint(-32768, 32767), rnd.randint(-32768, 32767))
        for _ in range(2 * symbol_len)
    ]
    await run_scenario(
        dut, FFT_8K, numerator, samples, rnd, gaps=True, backpressure=True
    )


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def fft_32k_scaling(dut):
    """FFT_32K: the largest symbol (scale 4), near the counter's bound."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x32C0)
    numerator = 4096
    symbol_len = FFT_32K + 4 * numerator
    samples = [
        (rnd.randint(-32768, 32767), rnd.randint(-32768, 32767))
        for _ in range(symbol_len + 8)
    ]
    await run_scenario(dut, FFT_32K, numerator, samples, rnd)


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def out_of_range_config_is_clamped(dut):
    """An undefined FFT size falls back to 8K and a CP numerator above
    4096 clamps to 4096, each raising its sticky flag; the stream then
    matches the golden model run at the clamped values."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xC1A3)
    symbol_len = FFT_8K + 4096
    samples = [
        (rnd.randint(-32768, 32767), rnd.randint(-32768, 32767))
        for _ in range(symbol_len + 8)
    ]
    await run_scenario(
        dut, 12345, 5000, samples, rnd, golden_cfg=(FFT_8K, 4096), expect_flags=(1, 1)
    )


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def reset_mid_symbol(dut):
    """A reset part-way through a symbol restarts framing from the next
    sample: the following stream matches a fresh golden run."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x5E7)
    await reset_dut(dut, FFT_8K, 1024)
    dut.m_axis_tready.value = 1
    partial = [
        (rnd.randint(-32768, 32767), rnd.randint(-32768, 32767)) for _ in range(5000)
    ]
    first = cocotb.start_soon(drive_input(dut, partial, rnd, False))
    for _ in range(3000):
        await RisingEdge(dut.clk)
    assert not first.done()
    first.kill()
    symbol_len = FFT_8K + 1024
    samples = [
        (rnd.randint(-32768, 32767), rnd.randint(-32768, 32767))
        for _ in range(symbol_len + 8)
    ]
    await run_scenario(dut, FFT_8K, 1024, samples, rnd)
