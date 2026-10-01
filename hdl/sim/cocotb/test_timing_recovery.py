"""cocotb testbench for hdl/rtl/sync/timing_recovery.v

Bit-exact comparison against the real golden model
(lib/sync/timing_recovery.cc, ATSC3_FIXED_POINT=ON) via the
hdl/sim/golden/timing_recovery_gen CLI -- never a Python
reimplementation. Two things are compared, both against every input
sample:
  * the monitor output (mu_q16, timing_error_q15) after each accepted
    sample, whether or not it happened to land on a symbol boundary;
  * every emitted m_axis symbol beat (value only -- interpolate() calls
    for the emitted symbol and the golden CLI's CLI process the same
    stream, so ordering is implicit).

kp_q15/ki_q15 are compute_loop_gains()'s one-time output for the test's
loop_bandwidth_hz/loop_damping/symbol_rate_hz -- the golden CLI reports
them (the "G" line) and the RTL is configured with those exact numbers
directly, per timing_recovery.v's header comment on why the derivation
itself isn't re-implemented in RTL.

Timing idiom: inputs are driven 1 ns after a rising edge and every DUT
output (handshakes, monitor) is sampled at the falling edge, so each
sample sees exactly what the next rising edge will see on both
simulators.

Run via test_runner.py (pytest), not directly.
"""

import random
import subprocess
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import FallingEdge, RisingEdge, Timer

REPO_ROOT = Path(__file__).resolve().parents[3]
TIMING_RECOVERY_GEN = (
    REPO_ROOT / "build-fxp" / "hdl" / "sim" / "golden" / "timing_recovery_gen"
)

DEFAULT_FS = 6_250_000
DEFAULT_SYMBOL_RATE_MILLIHZ = 718750
DEFAULT_LOOP_BW_HZ = 1
DEFAULT_LOOP_DAMPING_MILLI = 1000
DEFAULT_SPS = 2


def _clamp16(v):
    return max(-32768, min(32767, int(v)))


def _to_signed(v, bits):
    v = int(v)
    if v & (1 << (bits - 1)):
        v -= 1 << bits
    return v


def pack(re, im):
    return ((re & 0xFFFF) << 16) | (im & 0xFFFF)


def unpack32(word):
    return _to_signed((word >> 16) & 0xFFFF, 16), _to_signed(word & 0xFFFF, 16)


def run_golden(
    sample_rate_hz,
    symbol_rate_millihz,
    loop_bw_hz,
    loop_damping_milli,
    sps,
    initial_offset_q15,
    locked_at_start,
    samples,
    lock_changes=None,
):
    """lock_changes: {sample index: locked}, applied before that sample."""
    lock_changes = lock_changes or {}
    if not TIMING_RECOVERY_GEN.exists():
        raise FileNotFoundError(
            f"{TIMING_RECOVERY_GEN} not found -- build it first: "
            "cmake --build build-fxp --target timing_recovery_gen "
            "(requires -DATSC3_FIXED_POINT=ON -DATSC3_ENABLE_HDL_STUBS=ON)"
        )
    cfg_line = (
        f"C {sample_rate_hz} {symbol_rate_millihz} {loop_bw_hz} "
        f"{loop_damping_milli} {sps} {initial_offset_q15} {int(locked_at_start)}"
    )
    lines = [cfg_line]
    for i, (re, im) in enumerate(samples):
        if i in lock_changes:
            lines.append(f"L {int(lock_changes[i])}")
        lines.append(f"{re} {im}")
    proc = subprocess.run(
        [str(TIMING_RECOVERY_GEN)],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        check=True,
    )
    out = proc.stdout.splitlines()
    assert out[0].startswith("G "), f"expected G line first, got: {out[0]!r}"
    _, kp_q15, ki_q15 = out[0].split()
    kp_q15, ki_q15 = int(kp_q15), int(ki_q15)

    monitors = []  # one per input sample: (mu_q16, timing_error_q15)
    symbols = []  # emitted (re, im, mu_q16), in order
    for line in out[1:]:
        if line.startswith("M "):
            _, mu_q16, err_q15 = line.split()
            monitors.append((int(mu_q16), int(err_q15)))
        elif line.startswith("S "):
            _, re, im, mu_q16 = line.split()
            symbols.append((int(re), int(im), int(mu_q16)))
    return kp_q15, ki_q15, monitors, symbols


async def reset_dut(dut, kp_q15, ki_q15, sps, initial_offset_q15, locked):
    await Timer(1, "ns")
    dut.rst.value = 1
    dut.cfg_kp_q15.value = kp_q15 & 0xFFFF
    dut.cfg_ki_q15.value = ki_q15 & 0xFFFF
    dut.cfg_samples_per_symbol.value = sps
    dut.cfg_initial_offset_q15.value = initial_offset_q15 & 0xFFFF
    dut.cfg_locked.value = int(locked)
    dut.s_axis_tvalid.value = 0
    dut.s_axis_tdata.value = 0
    dut.s_axis_tlast.value = 0
    dut.m_axis_tready.value = 1
    for _ in range(3):
        await RisingEdge(dut.clk)
    await Timer(1, "ns")
    dut.rst.value = 0
    # ST_CLEAR sweeps the 128-entry buffer before s_axis_tready rises.
    for _ in range(200):
        await FallingEdge(dut.clk)
        if dut.s_axis_tready.value == 1:
            break
    assert dut.s_axis_tready.value == 1, "DUT never came out of reset/clear"


async def drive(dut, samples, rnd, gaps, lock_changes):
    for i, (re, im) in enumerate(samples):
        # cfg_locked is a live control: change it only between samples,
        # once the previous one is fully processed (s_axis_tready high),
        # which is where the golden model's set_locked() call sits.
        while dut.s_axis_tready.value != 1:
            await FallingEdge(dut.clk)
        if i in lock_changes:
            dut.cfg_locked.value = int(lock_changes[i])
        if gaps and rnd.random() < 0.2:
            for _ in range(rnd.randint(1, 5)):
                await RisingEdge(dut.clk)
            await FallingEdge(dut.clk)
        dut.s_axis_tdata.value = pack(re, im)
        dut.s_axis_tvalid.value = 1
        # s_axis_tready is registered, so its value now (mid-cycle) is the
        # one the next rising edge sees.
        while True:
            fire = dut.s_axis_tready.value == 1
            await RisingEdge(dut.clk)
            if fire:
                break
            await FallingEdge(dut.clk)
        await Timer(1, "ns")
        dut.s_axis_tvalid.value = 0


async def backpressure(dut, rnd):
    while True:
        await RisingEdge(dut.clk)
        await Timer(1, "ns")
        dut.m_axis_tready.value = int(rnd.random() < 0.6)


async def monitor(dut, mons, syms):
    while True:
        await FallingEdge(dut.clk)
        if dut.m_axis_tvalid.value == 1 and dut.m_axis_tready.value == 1:
            syms.append(unpack32(int(dut.m_axis_tdata.value)))
        if dut.mon_valid.value == 1:
            mons.append(
                (
                    int(dut.mon_mu_q16.value),
                    _to_signed(int(dut.mon_timing_error_q15.value), 64),
                )
            )


async def run_scenario(
    dut,
    samples,
    seed,
    sps=DEFAULT_SPS,
    initial_offset_q15=0,
    locked=True,
    loop_bw_hz=DEFAULT_LOOP_BW_HZ,
    loop_damping_milli=DEFAULT_LOOP_DAMPING_MILLI,
    lock_changes=None,
    gaps=False,
    stall_output=False,
    expect_mu_motion=False,
):
    lock_changes = lock_changes or {}
    kp_q15, ki_q15, monitors, symbols = run_golden(
        DEFAULT_FS,
        DEFAULT_SYMBOL_RATE_MILLIHZ,
        loop_bw_hz,
        loop_damping_milli,
        sps,
        initial_offset_q15,
        locked,
        samples,
        lock_changes,
    )
    assert len(monitors) == len(samples)
    assert symbols, "stimulus emitted no golden symbols -- scenario proves nothing"
    if expect_mu_motion:
        assert (
            len({mu for mu, _ in monitors}) > 4
        ), "loop never moved mu -- raise loop bandwidth"

    await reset_dut(dut, kp_q15, ki_q15, sps, initial_offset_q15, locked)
    rnd = random.Random(seed)
    mons, syms = [], []
    mon_task = cocotb.start_soon(monitor(dut, mons, syms))
    bp_task = cocotb.start_soon(backpressure(dut, rnd)) if stall_output else None
    await drive(dut, samples, rnd, gaps, lock_changes)
    for _ in range(2000):  # last sample's processing and symbol drain
        await RisingEdge(dut.clk)
        if len(mons) == len(samples) and len(syms) >= len(symbols):
            break
    mon_task.kill()
    if bp_task:
        bp_task.kill()

    for i, (got, exp) in enumerate(zip(mons, monitors)):
        assert (
            got == exp
        ), f"sample {i}: (mu_q16, timing_error_q15) expected {exp}, got {got}"
    assert len(mons) == len(
        monitors
    ), f"{len(mons)} monitor updates for {len(monitors)} samples"
    for k, (got, exp) in enumerate(zip(syms, symbols)):
        assert got == exp[:2], f"symbol {k}: expected {exp[:2]}, got {got}"
    assert len(syms) == len(
        symbols
    ), f"RTL emitted {len(syms)} symbols, golden {len(symbols)}"


def noise(rnd, n, sigma=8000):
    return [
        (_clamp16(rnd.gauss(0, sigma)), _clamp16(rnd.gauss(0, sigma))) for _ in range(n)
    ]


@cocotb.test(timeout_time=50, timeout_unit="ms")
async def acquisition_and_tracking(dut):
    """Buffer fill, then locked tracking with a loop fast enough to move mu
    across several polyphase phases, under input gaps and output
    backpressure."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x71171)
    await run_scenario(
        dut,
        noise(rnd, 600),
        1,
        loop_bw_hz=50000,
        gaps=True,
        stall_output=True,
        expect_mu_motion=True,
    )


@cocotb.test(timeout_time=20, timeout_unit="ms")
async def unlocked_no_loop_update(dut):
    """With cfg_locked=0, symbols still emit once the buffer fills, but
    mu_q16/timing_error_q15 must stay exactly at their reset values --
    the TED/loop-filter path never engages."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xD0104)
    await run_scenario(dut, noise(rnd, 80), 2, locked=False)


@cocotb.test(timeout_time=20, timeout_unit="ms")
async def nonzero_initial_offset(dut):
    """A nonzero, negative-signed initial_offset_q15 exercises the
    Q1.15->Q0.16 reset-time conversion's wraparound case."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x9FF5E7)
    # -8000 in Q1.15 (~ -0.244), well clear of both 0 and the [0,0.5)/
    # [0.5,1) wrap boundary.
    await run_scenario(dut, noise(rnd, 80), 3, initial_offset_q15=-8000, locked=True)


@cocotb.test(timeout_time=50, timeout_unit="ms")
async def lock_toggled_mid_stream(dut):
    """cfg_locked is read live: unlock and relock mid-stream (lock loss
    and reacquisition) and stay bit-exact through both transitions."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x10C4)
    await run_scenario(
        dut,
        noise(rnd, 300),
        4,
        loop_bw_hz=50000,
        lock_changes={100: False, 200: True},
        expect_mu_motion=True,
    )


@cocotb.test(timeout_time=50, timeout_unit="ms")
async def other_oversampling_ratios(dut):
    """samples_per_symbol 3 (odd: the TED midpoint index rounds down) and
    4, each with tracking enabled."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x5B5)
    for sps in (3, 4):
        await run_scenario(
            dut, noise(rnd, 240), 5 + sps, sps=sps, loop_bw_hz=50000, stall_output=True
        )


@cocotb.test(timeout_time=50, timeout_unit="ms")
async def reset_mid_stream(dut):
    """A reset while a sample is mid-processing must return the block to
    its power-on state: the following stream is bit-exact against a fresh
    golden run (buffer cleared, loop state and counters reset)."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x7E5E7)
    kp_q15, ki_q15, _, _ = run_golden(
        DEFAULT_FS,
        DEFAULT_SYMBOL_RATE_MILLIHZ,
        50000,
        DEFAULT_LOOP_DAMPING_MILLI,
        DEFAULT_SPS,
        0,
        True,
        [],
    )
    await reset_dut(dut, kp_q15, ki_q15, DEFAULT_SPS, 0, True)
    first = cocotb.start_soon(drive(dut, noise(rnd, 120), rnd, False, {}))
    for _ in range(rnd.randint(3000, 6000)):
        await RisingEdge(dut.clk)
    assert not first.done(), "first stream finished before the reset -- lengthen it"
    first.kill()
    await run_scenario(dut, noise(rnd, 200), 9, loop_bw_hz=50000, gaps=True)
