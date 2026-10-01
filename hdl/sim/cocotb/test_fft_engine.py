"""cocotb testbench for hdl/rtl/ofdm/fft_engine.v

Bit-exact comparison against the real golden model
(lib/ofdm/fft_engine.cc's FixedPointFftEngine, ATSC3_FIXED_POINT=ON) via
the hdl/sim/golden/fft_engine_gen CLI -- never a Python reimplementation
of the Cooley-Tukey radix-2 DIT. Forward direction only (see
fft_engine.v's header on why there is no direction config).

Runs full transforms at real FFT_SIZE -- correctness at the real size,
with the real ROM, is the point. 8192 and 16384 run every time; 32768
(about 0.8M cycles per transform) runs when HDL_FULL=1 (nightly).
Covers back-to-back transforms with input gaps and output backpressure,
an undefined size falling back to 8K, and resets during load and
during the butterfly stages.

Run via test_runner.py (pytest), not directly.
"""

import os
import random
import subprocess
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge

REPO_ROOT = Path(__file__).resolve().parents[3]
FULL = os.environ.get("HDL_FULL") == "1"
FFT_ENGINE_GEN = REPO_ROOT / "build-fxp" / "hdl" / "sim" / "golden" / "fft_engine_gen"


def _to_signed(v, bits):
    v = int(v)
    if v & (1 << (bits - 1)):
        v -= 1 << bits
    return v


def pack(re, im):
    return ((re & 0xFFFF) << 16) | (im & 0xFFFF)


def unpack32(word):
    return _to_signed((word >> 16) & 0xFFFF, 16), _to_signed(word & 0xFFFF, 16)


def run_golden(fft_size, samples):
    if not FFT_ENGINE_GEN.exists():
        raise FileNotFoundError(
            f"{FFT_ENGINE_GEN} not found -- build it first: "
            "cmake --build build-fxp --target fft_engine_gen "
            "(requires -DATSC3_FIXED_POINT=ON -DATSC3_ENABLE_HDL_STUBS=ON)"
        )
    lines = [f"C {fft_size}"] + [f"{re} {im}" for re, im in samples]
    proc = subprocess.run(
        [str(FFT_ENGINE_GEN)],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        check=True,
    )
    out = []
    for line in proc.stdout.splitlines():
        re, im = line.split()
        out.append((int(re), int(im)))
    return out


async def reset_dut(dut, fft_size, expect_invalid=0):
    dut.rst.value = 1
    dut.cfg_fft_size.value = fft_size
    dut.s_axis_tvalid.value = 0
    dut.s_axis_tdata.value = 0
    dut.s_axis_tlast.value = 0
    dut.m_axis_tready.value = 1
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst.value = 0
    await RisingEdge(dut.clk)
    assert dut.cfg_fft_size_invalid.value == expect_invalid


async def drive_input(dut, samples, rnd=None):
    for re, im in samples:
        if rnd is not None and rnd.random() < 0.02:
            dut.s_axis_tvalid.value = 0
            for _ in range(rnd.randint(1, 4)):
                await RisingEdge(dut.clk)
        dut.s_axis_tvalid.value = 1
        dut.s_axis_tdata.value = pack(re, im)
        while True:
            await RisingEdge(dut.clk)
            if dut.s_axis_tready.value == 1:
                break
    dut.s_axis_tvalid.value = 0


async def drive_backpressure(dut, rnd):
    while True:
        dut.m_axis_tready.value = int(rnd.random() < 0.7)
        await RisingEdge(dut.clk)


async def collect_output(dut, expected):
    """expected: list of (re, im, last). Also checks the AXI4-S hold rule:
    a stalled beat keeps its data until accepted."""
    idx = 0
    stalled = None
    while idx < len(expected):
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value != 1:
            assert (
                stalled is None
            ), f"output {idx}: TVALID dropped before the beat was accepted"
            continue
        word = (int(dut.m_axis_tdata.value), int(dut.m_axis_tlast.value))
        if stalled is not None:
            assert word == stalled, f"output {idx}: data changed while stalled"
        if dut.m_axis_tready.value != 1:
            stalled = word
            continue
        stalled = None
        got = unpack32(word[0])
        exp_re, exp_im, exp_last = expected[idx]
        assert got == (
            exp_re,
            exp_im,
        ), f"output {idx}: expected {(exp_re, exp_im)}, got {got}"
        assert word[1] == int(exp_last), f"output {idx}: tlast expected {int(exp_last)}"
        idx += 1


async def run_stream(
    dut,
    fft_size,
    transforms,
    rnd=None,
    backpressure=False,
    cfg_size=None,
    expect_invalid=0,
):
    """Back-to-back transforms through one reset; rnd adds input gaps."""
    expected = []
    for samples in transforms:
        out = run_golden(fft_size, samples)
        assert len(out) == fft_size
        expected += [(re, im, k == fft_size - 1) for k, (re, im) in enumerate(out)]

    await reset_dut(dut, cfg_size or fft_size, expect_invalid)
    bp_task = cocotb.start_soon(drive_backpressure(dut, rnd)) if backpressure else None
    drive_task = cocotb.start_soon(
        drive_input(dut, [x for samples in transforms for x in samples], rnd)
    )
    await collect_output(dut, expected)
    await drive_task
    if bp_task is not None:
        bp_task.kill()
        dut.m_axis_tready.value = 1


def random_samples(rnd, n):
    return [(rnd.randint(-16384, 16383), rnd.randint(-16384, 16383)) for _ in range(n)]


@cocotb.test(timeout_time=100, timeout_unit="ms")
async def impulse_8k(dut):
    """A unit impulse must produce a perfectly flat spectrum -- the
    simplest possible cross-check of the whole load/transform/unload
    pipeline (bit-reversal, all 13 stages, saturation)."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    samples = [(0, 0)] * 8192
    samples[0] = (32767, 0)
    await run_stream(dut, 8192, [samples])


@cocotb.test(timeout_time=300, timeout_unit="ms")
async def back_to_back_8k_with_stalls(dut):
    """Two random transforms with no reset between, random input gaps and
    random output stalls: the next load overlaps the previous transform's
    last pending beat, and every stalled beat must hold."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xF77123)
    await run_stream(
        dut,
        8192,
        [random_samples(rnd, 8192), random_samples(rnd, 8192)],
        rnd=rnd,
        backpressure=True,
    )


@cocotb.test(timeout_time=300, timeout_unit="ms")
async def random_16k(dut):
    """The 14-stage case: the stage/ROM-addressing algebra beyond 8K."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x16C0DE)
    await run_stream(dut, 16384, [random_samples(rnd, 16384)])


@cocotb.test(timeout_time=1000, timeout_unit="ms", skip=not FULL)
async def random_32k(dut):
    """The 15-stage case at the largest size (nightly: HDL_FULL=1)."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x32C0DE)
    await run_stream(dut, 32768, [random_samples(rnd, 32768)])


@cocotb.test(timeout_time=100, timeout_unit="ms")
async def undefined_size_falls_back_to_8k(dut):
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xBAD5)
    await run_stream(
        dut, 8192, [random_samples(rnd, 8192)], cfg_size=12000, expect_invalid=1
    )


@cocotb.test(timeout_time=300, timeout_unit="ms")
async def reset_during_load_and_butterflies(dut):
    """Resets part-way through loading and part-way through the butterfly
    stages; each time the next transform must be bit-exact from scratch."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x5E7F)
    for wait_cycles in (3000, 8192 + 20000):
        await reset_dut(dut, 8192)
        task = cocotb.start_soon(drive_input(dut, random_samples(rnd, 8192)))
        for _ in range(wait_cycles):
            await RisingEdge(dut.clk)
        task.kill()
        await run_stream(dut, 8192, [random_samples(rnd, 8192)])
