"""cocotb testbench for hdl/rtl/sync/freq_correction.v

Bit-exact comparison against the real golden model
(lib/sync/freq_correction.cc's FreqCorrection::process(),
ATSC3_FIXED_POINT=ON) via the hdl/sim/golden/freq_correction_gen CLI. The
CLI also runs the control plane (Hz to Q1.31 increment, fine-CFO loop
filter) and reports each new increment, which the bench programs into
cfg_phase_inc before the next sample, as software would.

Covers a CFO sweep from 0 to near +/-Fs/2 on full-scale random samples
(saturation, many accumulator wraps), fine-CFO updates mid-stream,
phase resets both between samples and on the edge that accepts a sample,
TLAST passing through, random input gaps and output backpressure with the
AXI4-S hold rule checked, and a reset while a sample is in flight.
cfo_injected_tone also checks the correction itself against an
independent expectation: a tone at +f, corrected for a CFO of f, comes
out within 0.2% of full scale of a constant.

Run via test_runner.py (pytest), not directly.
"""

import cmath
import math
import random
import subprocess
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import FallingEdge, RisingEdge

REPO_ROOT = Path(__file__).resolve().parents[3]
FREQ_CORRECTION_GEN = (
    REPO_ROOT / "build-fxp" / "hdl" / "sim" / "golden" / "freq_correction_gen"
)
FS = 6.25e6
LATENCY = 18


def _to_signed(v, bits):
    v = int(v)
    if v & (1 << (bits - 1)):
        v -= 1 << bits
    return v


def pack(re, im):
    return ((re & 0xFFFF) << 16) | (im & 0xFFFF)


def unpack(word):
    word = int(word)
    return _to_signed((word >> 16) & 0xFFFF, 16), _to_signed(word & 0xFFFF, 16)


def run_golden(events):
    """events: ("F", hz) | ("U", rad_per_sample) | ("R",) | ("S", re, im, last).
    Returns the same list with F/U replaced by ("I", increment) and each
    sample extended with its corrected output."""
    if not FREQ_CORRECTION_GEN.exists():
        raise FileNotFoundError(
            f"{FREQ_CORRECTION_GEN} not found -- build it first: "
            "cmake --build build-fxp --target freq_correction_gen "
            "(requires -DATSC3_FIXED_POINT=ON -DATSC3_ENABLE_HDL_STUBS=ON)"
        )
    lines = [f"C {FS}"]
    for ev in events:
        if ev[0] in ("F", "U"):
            lines.append(f"{ev[0]} {ev[1]!r}")
        elif ev[0] == "R":
            lines.append("R")
        else:
            lines.append(f"{ev[1]} {ev[2]}")
    proc = subprocess.run(
        [str(FREQ_CORRECTION_GEN)],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        check=True,
    )
    out_lines = proc.stdout.splitlines()
    assert len(out_lines) == len(events)
    result = []
    for ev, line in zip(events, out_lines):
        if ev[0] in ("F", "U"):
            tag, inc = line.split()
            assert tag == "I"
            result.append(("I", int(inc)))
        elif ev[0] == "R":
            assert line == "R"
            result.append(ev)
        else:
            re, im = (int(v) for v in line.split())
            result.append(("S", ev[1], ev[2], ev[3], (re, im)))
    return result


async def reset_dut(dut):
    dut.rst.value = 1
    dut.cfg_phase_inc.value = 0
    dut.phase_reset.value = 0
    dut.s_axis_tvalid.value = 0
    dut.s_axis_tdata.value = 0
    dut.s_axis_tlast.value = 0
    dut.m_axis_tready.value = 1
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst.value = 0
    await RisingEdge(dut.clk)


async def drive(dut, script, rnd, gaps, reset_with_sample):
    """Plays the golden script: increments are programmed before the next
    sample; a phase reset is either a one-cycle pulse between samples or,
    with reset_with_sample, held high on the edge that accepts the next
    sample."""
    pending_reset = False
    for ev in script:
        if ev[0] == "I":
            dut.cfg_phase_inc.value = ev[1] & 0xFFFFFFFF
        elif ev[0] == "R":
            if reset_with_sample:
                pending_reset = True
            else:
                dut.phase_reset.value = 1
                await RisingEdge(dut.clk)
                dut.phase_reset.value = 0
        else:
            _, re, im, last, _ = ev
            if gaps and rnd.random() < 0.3:
                for _ in range(rnd.randint(1, 25)):
                    await RisingEdge(dut.clk)
            dut.s_axis_tvalid.value = 1
            dut.s_axis_tdata.value = pack(re, im)
            dut.s_axis_tlast.value = int(last)
            while True:
                # tready only changes on a rising edge, so its value at the
                # falling edge is what the coming rising edge sees: hold
                # phase_reset on the accepting edge only.
                await FallingEdge(dut.clk)
                accepting = dut.s_axis_tready.value == 1
                dut.phase_reset.value = int(pending_reset and accepting)
                await RisingEdge(dut.clk)
                if accepting:
                    break
            pending_reset = False
            dut.phase_reset.value = 0
            dut.s_axis_tvalid.value = 0


async def drive_backpressure(dut, rnd):
    """Mostly short random stalls, sometimes one longer than a whole
    sample (LATENCY cycles), so a finished sample meets a stalled output."""
    while True:
        if rnd.random() < 0.05:
            dut.m_axis_tready.value = 0
            for _ in range(rnd.randint(LATENCY + 1, 3 * LATENCY)):
                await RisingEdge(dut.clk)
        dut.m_axis_tready.value = int(rnd.random() < 0.5)
        await RisingEdge(dut.clk)


async def collect(dut, expected):
    """expected: list of ((re, im), last). Checks the hold rule."""
    out = []
    stalled = None
    while len(out) < len(expected):
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value != 1:
            assert stalled is None, f"output {len(out)}: TVALID dropped while stalled"
            continue
        word = (int(dut.m_axis_tdata.value), int(dut.m_axis_tlast.value))
        if stalled is not None:
            assert word == stalled, f"output {len(out)}: data changed while stalled"
        if dut.m_axis_tready.value != 1:
            stalled = word
            continue
        stalled = None
        i = len(out)
        got = unpack(word[0])
        assert (
            got == expected[i][0]
        ), f"output {i}: expected {expected[i][0]}, got {got}"
        assert word[1] == int(expected[i][1]), f"output {i}: TLAST"
        out.append(got)
    return out


async def run_script(
    dut, events, rnd, gaps=False, stalls=False, reset_with_sample=False
):
    script = run_golden(events)
    expected = [(ev[4], ev[3]) for ev in script if ev[0] == "S"]
    await reset_dut(dut)
    bp = cocotb.start_soon(drive_backpressure(dut, rnd)) if stalls else None
    drv = cocotb.start_soon(drive(dut, script, rnd, gaps, reset_with_sample))
    out = await collect(dut, expected)
    await drv
    if bp is not None:
        bp.kill()
        dut.m_axis_tready.value = 1
    for _ in range(LATENCY + 4):
        await RisingEdge(dut.clk)
    assert dut.m_axis_tvalid.value == 0, "extra output after the last expected one"
    return out


def random_samples(rnd, n, last_every=0):
    full = [-32768, 32767]
    out = []
    for i in range(n):
        if rnd.random() < 0.1:
            re, im = rnd.choice(full), rnd.choice(full)
        else:
            re, im = rnd.randint(-32768, 32767), rnd.randint(-32768, 32767)
        last = last_every > 0 and i % last_every == last_every - 1
        out.append(("S", re, im, last))
    return out


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def cfo_sweep(dut):
    """CFOs from 0 to near +/-Fs/2 (a full turn every two samples) on
    full-scale random data; TLAST every 7th sample."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xCF0)
    events = []
    for hz in (0.0, 500.3, -12345.6, 1.0e6, -2.9e6, 3.1e6):
        events.append(("F", hz))
        events += random_samples(rnd, 120, last_every=7)
    await run_script(dut, events, rnd)


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def fine_updates_and_phase_resets(dut):
    """Fine-CFO updates mid-stream and phase resets between samples."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xF1E)
    events = [("F", 25000.0)] + random_samples(rnd, 60)
    for err in (0.5, -0.25, 3.0):
        events.append(("U", err))
        events += random_samples(rnd, 40)
        events.append(("R",))
        events += random_samples(rnd, 40)
    await run_script(dut, events, rnd)


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def phase_reset_on_accepting_edge(dut):
    """phase_reset high on the edge that accepts a sample: that sample is
    rotated by phase 0 (reset_phase() before process())."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xE5E)
    events = [("F", 777777.0)] + random_samples(rnd, 30)
    for _ in range(5):
        events.append(("R",))
        events += random_samples(rnd, 13)
    await run_script(dut, events, rnd, reset_with_sample=True)


@cocotb.test(timeout_time=300, timeout_unit="ms")
async def gaps_and_backpressure(dut):
    """Random input gaps and output stalls, some longer than a sample's
    latency so a finished sample waits on a stalled output; every stalled
    beat holds."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xB9E)
    events = [("F", -400000.0)] + random_samples(rnd, 300, last_every=5)
    await run_script(dut, events, rnd, gaps=True, stalls=True)


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def reset_mid_sample(dut):
    """Reset while the CORDIC is busy, then a clean run from phase 0."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x5E7)
    await reset_dut(dut)
    dut.cfg_phase_inc.value = 123456789
    for i in range(3):
        dut.s_axis_tvalid.value = 1
        dut.s_axis_tdata.value = pack(1000 * i, -1000 * i)
        await RisingEdge(dut.clk)
        while dut.s_axis_tready.value != 1:
            await RisingEdge(dut.clk)
    dut.s_axis_tvalid.value = 0
    for _ in range(7):
        await RisingEdge(dut.clk)
    events = [("F", 300000.0)] + random_samples(rnd, 50)
    await run_script(dut, events, rnd)


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def cfo_injected_tone(dut):
    """Independent check: a tone at +f (amplitude 0.5) corrected for a CFO
    of f stays within 0.2% of full scale of its first output sample."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x70E)
    f = 48000.0
    events = [("F", f)]
    for n in range(400):
        z = 16384 * cmath.exp(2j * math.pi * f * n / FS)
        events.append(("S", round(z.real), round(z.imag), False))
    out = await run_script(dut, events, rnd)
    ref = complex(*out[0])
    worst = max(abs(complex(*o) - ref) for o in out)
    assert worst < 0.002 * 32768, f"residual rotation: worst deviation {worst:.1f} LSB"
