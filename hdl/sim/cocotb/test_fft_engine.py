"""cocotb testbench for hdl/rtl/ofdm/fft_engine.v

Bit-exact comparison against the real golden model
(lib/ofdm/fft_engine.cc's FixedPointFftEngine, ATSC3_FIXED_POINT=ON) via
the hdl/sim/golden/fft_engine_gen CLI -- never a Python reimplementation
of the Cooley-Tukey radix-2 DIT. Forward direction only (see
fft_engine.v's header on why there is no direction config).

Runs full transforms at real FFT_SIZE (8192 and 16384) -- this block has
no small-parameterization escape hatch the way a control-only formal
proof does; correctness at the real size, with the real ROM, is the
point. 32768 is left to the formal proof's structural (not full-
transform) bit-reversal/addressing checks plus these two smaller sizes
already exercising every stage-count case the FSM has (8192 uses 13
stages, 16384 uses 14 -- 32768's 15th stage is the same butterfly/ROM-
addressing logic one more time, not a new code path).

Run via test_runner.py (pytest), not directly.
"""

import random
import subprocess
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge

REPO_ROOT = Path(__file__).resolve().parents[3]
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
        [str(FFT_ENGINE_GEN)], input="\n".join(lines) + "\n",
        capture_output=True, text=True, check=True,
    )
    out = []
    for line in proc.stdout.splitlines():
        re, im = line.split()
        out.append((int(re), int(im)))
    return out


async def reset_dut(dut, fft_size):
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
    assert dut.cfg_fft_size_invalid.value == 0


async def drive_input(dut, samples):
    for re, im in samples:
        dut.s_axis_tvalid.value = 1
        dut.s_axis_tdata.value = pack(re, im)
        while True:
            await RisingEdge(dut.clk)
            if dut.s_axis_tready.value == 1:
                break
    dut.s_axis_tvalid.value = 0


async def collect_output(dut, expected, done_event):
    idx = 0
    while idx < len(expected):
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value == 1 and dut.m_axis_tready.value == 1:
            got = unpack32(int(dut.m_axis_tdata.value))
            exp = expected[idx]
            assert got == exp, f"output {idx}: expected {exp}, got {got}"
            exp_last = (idx == len(expected) - 1)
            got_last = int(dut.m_axis_tlast.value)
            assert got_last == int(exp_last), (
                f"output {idx}: tlast expected {int(exp_last)}, got {got_last}"
            )
            idx += 1
    done_event.set()


async def run_transform(dut, fft_size, samples):
    expected = run_golden(fft_size, samples)
    assert len(expected) == fft_size

    await reset_dut(dut, fft_size)

    done_event = cocotb.triggers.Event()
    drive_task = cocotb.start_soon(drive_input(dut, samples))
    await collect_output(dut, expected, done_event)
    await drive_task


@cocotb.test()
async def impulse_8k(dut):
    """A unit impulse must produce a perfectly flat spectrum -- the
    simplest possible cross-check of the whole load/transform/unload
    pipeline (bit-reversal, all 13 stages, saturation) before the
    randomized tests."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())

    fft_size = 8192
    samples = [(0, 0)] * fft_size
    samples[0] = (32767, 0)
    await run_transform(dut, fft_size, samples)


@cocotb.test()
async def random_8k(dut):
    """Full bit-exact transform of random input at the default data FFT
    size."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xF77123)

    fft_size = 8192
    samples = [(rnd.randint(-16384, 16383), rnd.randint(-16384, 16383)) for _ in range(fft_size)]
    await run_transform(dut, fft_size, samples)


@cocotb.test()
async def random_16k(dut):
    """A second, larger size to exercise the 14-stage case and confirm
    the stage/ROM-addressing algebra generalizes beyond 8K (see this
    file's header for why 32K is left to the formal proof)."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x16C0DE)

    fft_size = 16384
    samples = [(rnd.randint(-16384, 16383), rnd.randint(-16384, 16383)) for _ in range(fft_size)]
    await run_transform(dut, fft_size, samples)
