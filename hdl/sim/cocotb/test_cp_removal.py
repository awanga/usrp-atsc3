# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Alfred Wanga
#
"""cocotb testbench for hdl/rtl/ofdm/cp_removal.v

Bit-exact comparison against the real golden model (lib/ofdm/cp_removal.cc,
ATSC3_FIXED_POINT=ON) via the hdl/sim/golden/cp_removal_gen CLI -- never a
Python reimplementation of the guard interval lookup. The RTL
streams straight through (no internal buffering) while the C++ reference
buffers a whole CP+FFT symbol before its callback fires (see
cp_removal.v's header comment); both emit the identical value sequence
and framing, just at different latency, so the comparison is against that
value/TLAST sequence, not cycle-exact timing.

Covers every A/322 Table 9.13 guard interval code at an FFT size Table
8.9 allows it with (GI1-GI7 at 8K, GI8-GI11 at 16K, GI12 at 32K),
multi-symbol continuity, randomized backpressure on both sides of the
interface, reserved and disallowed configs falling back with their flags,
and a reset mid-symbol. Every scenario also counts output
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

# A/322 Table 8.9 length per Table 9.13 code (GI1_192 .. GI12_4864)
GI_SAMPLES = {
    1: 192, 2: 384, 3: 512, 4: 768, 5: 1024, 6: 1536,
    7: 2048, 8: 2432, 9: 3072, 10: 3648, 11: 4096, 12: 4864,
}
GI5_1024 = 5

FFT_8K = 8192
FFT_16K = 16384
FFT_32K = 32768


def _clamp16(v):
    return max(-32768, min(32767, int(v)))


def run_golden(fft_size, gi_code, samples):
    if not CP_REMOVAL_GEN.exists():
        raise FileNotFoundError(
            f"{CP_REMOVAL_GEN} not found -- build it first: "
            "cmake --build build-fxp --target cp_removal_gen "
            "(requires -DATSC3_FIXED_POINT=ON -DATSC3_ENABLE_HDL_STUBS=ON)"
        )
    lines = [f"C {fft_size} {gi_code}"] + [
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


async def reset_dut(dut, fft_size, gi_code):
    dut.rst.value = 1
    dut.cfg_fft_size.value = fft_size
    dut.cfg_guard_interval.value = gi_code
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
    gi_code,
    samples,
    rnd,
    gaps=False,
    backpressure=False,
    golden_cfg=None,
    expect_flags=(0, 0),
):
    """golden_cfg: the (fft_size, gi_code) the golden model should use,
    when the RTL is given an invalid config it must fall back to that."""
    await reset_dut(dut, fft_size, gi_code)

    golden = run_golden(*(golden_cfg or (fft_size, gi_code)), samples)
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
        int(dut.cfg_guard_interval_invalid.value),
    ) == expect_flags


def random_samples(rnd, n):
    return [
        (rnd.randint(-32768, 32767), rnd.randint(-32768, 32767)) for _ in range(n)
    ]


@cocotb.test(timeout_time=400, timeout_unit="ms")
async def every_guard_interval(dut):
    """Each Table 9.13 code once, one full symbol plus a short idle tail,
    at an FFT size Table 8.9 allows it with. GI8-GI12 are only allowed
    with 16K/32K, so the length must not scale with the FFT size."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xCB0192)
    for code, fft_size in [(c, FFT_8K) for c in range(1, 8)] + [
        (c, FFT_16K) for c in range(8, 12)
    ] + [(12, FFT_32K)]:
        samples = random_samples(rnd, fft_size + GI_SAMPLES[code] + 8)
        await run_scenario(dut, fft_size, code, samples, rnd)


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def same_length_at_every_fft_size(dut):
    """GI5_1024 is 1024 samples at 8K, 16K and 32K alike."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xF16C)
    for fft_size in (FFT_8K, FFT_16K, FFT_32K):
        samples = random_samples(rnd, fft_size + 1024 + 8)
        await run_scenario(dut, fft_size, GI5_1024, samples, rnd)


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def multi_symbol_continuity(dut):
    """Three consecutive symbols back to back at GI5_1024, checking the
    counter restarts cleanly at each TLAST."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xC0117)
    samples = random_samples(rnd, 3 * (FFT_8K + 1024))
    await run_scenario(dut, FFT_8K, GI5_1024, samples, rnd)


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def randomized_backpressure(dut):
    """Random input gaps and random output stalls across two symbols --
    the black-box counterpart to the formal no-drop/framing proofs."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xBACC9E)
    samples = random_samples(rnd, 2 * (FFT_8K + 512))
    await run_scenario(dut, FFT_8K, 3, samples, rnd, gaps=True, backpressure=True)


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def invalid_config_falls_back(dut):
    """An undefined FFT size falls back to 8K; a reserved guard interval
    code, or one Table 8.9 does not allow with the FFT size, falls back to
    GI5_1024. Each raises its sticky flag, and the stream matches the
    golden model run at the fallback."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xC1A3)
    cases = [
        (12345, 13, (FFT_8K, GI5_1024), (1, 1)),
        (FFT_8K, 0, (FFT_8K, GI5_1024), (0, 1)),
        (FFT_8K, 8, (FFT_8K, GI5_1024), (0, 1)),  # GI8_2432: N/A at 8K
        (FFT_16K, 12, (FFT_16K, GI5_1024), (0, 1)),  # GI12_4864: 32K only
        (12345, 8, (FFT_8K, GI5_1024), (1, 1)),  # GI8 judged against the 8K fallback
    ]
    for fft_size, code, golden_cfg, flags in cases:
        samples = random_samples(rnd, golden_cfg[0] + 1024 + 8)
        await run_scenario(
            dut, fft_size, code, samples, rnd, golden_cfg=golden_cfg, expect_flags=flags
        )


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def reset_mid_symbol(dut):
    """A reset part-way through a symbol restarts framing from the next
    sample: the following stream matches a fresh golden run."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x5E7)
    await reset_dut(dut, FFT_8K, GI5_1024)
    dut.m_axis_tready.value = 1
    partial = random_samples(rnd, 5000)
    first = cocotb.start_soon(drive_input(dut, partial, rnd, False))
    for _ in range(3000):
        await RisingEdge(dut.clk)
    assert not first.done()
    first.kill()
    samples = random_samples(rnd, FFT_8K + 1024 + 8)
    await run_scenario(dut, FFT_8K, GI5_1024, samples, rnd)
