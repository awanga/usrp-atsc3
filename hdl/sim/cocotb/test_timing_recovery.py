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

Run via test_runner.py (pytest), not directly.
"""

import random
import subprocess
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge

REPO_ROOT = Path(__file__).resolve().parents[3]
TIMING_RECOVERY_GEN = REPO_ROOT / "build-fxp" / "hdl" / "sim" / "golden" / "timing_recovery_gen"

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


def run_golden(sample_rate_hz, symbol_rate_millihz, loop_bw_hz, loop_damping_milli,
               sps, initial_offset_q15, locked_at_start, samples):
    if not TIMING_RECOVERY_GEN.exists():
        raise FileNotFoundError(
            f"{TIMING_RECOVERY_GEN} not found -- build it first: "
            "cmake --build build-fxp --target timing_recovery_gen "
            "(requires -DATSC3_FIXED_POINT=ON -DATSC3_ENABLE_HDL_STUBS=ON)"
        )
    cfg_line = (f"C {sample_rate_hz} {symbol_rate_millihz} {loop_bw_hz} "
                f"{loop_damping_milli} {sps} {initial_offset_q15} {int(locked_at_start)}")
    lines = [cfg_line] + [f"{re} {im}" for re, im in samples]
    proc = subprocess.run(
        [str(TIMING_RECOVERY_GEN)], input="\n".join(lines) + "\n",
        capture_output=True, text=True, check=True,
    )
    out = proc.stdout.splitlines()
    assert out[0].startswith("G "), f"expected G line first, got: {out[0]!r}"
    _, kp_q15, ki_q15 = out[0].split()
    kp_q15, ki_q15 = int(kp_q15), int(ki_q15)

    monitors = []   # one per input sample: (mu_q16, timing_error_q15)
    symbols = []    # emitted (re, im, mu_q16), in order
    for line in out[1:]:
        if line.startswith("M "):
            _, mu_q16, err_q15 = line.split()
            monitors.append((int(mu_q16), int(err_q15)))
        elif line.startswith("S "):
            _, re, im, mu_q16 = line.split()
            symbols.append((int(re), int(im), int(mu_q16)))
    return kp_q15, ki_q15, monitors, symbols


async def reset_dut(dut, kp_q15, ki_q15, sps, initial_offset_q15, locked):
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
    dut.rst.value = 0
    # ST_CLEAR sweeps the 128-entry buffer before s_axis_tready rises.
    for _ in range(200):
        await RisingEdge(dut.clk)
        if dut.s_axis_tready.value == 1:
            break
    assert dut.s_axis_tready.value == 1, "DUT never came out of reset/clear"


async def run_scenario(dut, samples, sps=DEFAULT_SPS, initial_offset_q15=0, locked=True,
                       loop_bw_hz=DEFAULT_LOOP_BW_HZ, loop_damping_milli=DEFAULT_LOOP_DAMPING_MILLI):
    kp_q15, ki_q15, monitors, symbols = run_golden(
        DEFAULT_FS, DEFAULT_SYMBOL_RATE_MILLIHZ, loop_bw_hz, loop_damping_milli,
        sps, initial_offset_q15, locked, samples,
    )
    assert len(monitors) == len(samples)

    await reset_dut(dut, kp_q15, ki_q15, sps, initial_offset_q15, locked)

    sym_idx = 0
    for i, (re, im) in enumerate(samples):
        dut.s_axis_tvalid.value = 1
        dut.s_axis_tdata.value = pack(re, im)
        # s_axis_tready is only ever raised (in ST_DONE_SAMPLE) after the
        # previous sample's mon_valid/m_axis_tvalid have already been
        # drained by the wait loop below, so it is already 1 by the time
        # we get here and this fires on the very first edge.
        while True:
            await RisingEdge(dut.clk)
            if dut.s_axis_tvalid.value == 1 and dut.s_axis_tready.value == 1:
                break
        dut.s_axis_tvalid.value = 0

        # This sample was just accepted on the edge above; wait for its
        # mon_valid pulse (issued once the FSM finishes processing it)
        # and check against monitors[i], draining any symbol beat first.
        mon_seen = False
        for _ in range(300):
            await RisingEdge(dut.clk)
            if dut.m_axis_tvalid.value == 1:
                got_re, got_im = unpack32(int(dut.m_axis_tdata.value))
                assert sym_idx < len(symbols), "RTL emitted more symbols than golden model"
                exp_re, exp_im, _exp_mu = symbols[sym_idx]
                assert (got_re, got_im) == (exp_re, exp_im), (
                    f"symbol {sym_idx}: expected ({exp_re},{exp_im}), got ({got_re},{got_im})"
                )
                sym_idx += 1
            if dut.mon_valid.value == 1:
                got_mu = int(dut.mon_mu_q16.value)
                got_err = _to_signed(int(dut.mon_timing_error_q15.value), 64)
                exp_mu, exp_err = monitors[i]
                assert got_mu == exp_mu, f"sample {i}: mu_q16 expected {exp_mu}, got {got_mu}"
                assert got_err == exp_err, (
                    f"sample {i}: timing_error_q15 expected {exp_err}, got {got_err}"
                )
                mon_seen = True
                break
        assert mon_seen, f"sample {i}: mon_valid never pulsed"

    assert sym_idx == len(symbols), f"expected {len(symbols)} symbols total, RTL emitted {sym_idx}"


@cocotb.test()
async def acquisition_and_tracking(dut):
    """Buffer fill, then locked tracking over enough boundaries to
    exercise the loop filter and several distinct polyphase phases."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x71171)

    samples = [(_clamp16(rnd.gauss(0, 8000)), _clamp16(rnd.gauss(0, 8000))) for _ in range(160)]
    await run_scenario(dut, samples, locked=True)


@cocotb.test()
async def unlocked_no_loop_update(dut):
    """With cfg_locked=0, symbols still emit once the buffer fills, but
    mu_q16/timing_error_q15 must stay exactly at their reset values --
    the TED/loop-filter path never engages."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0xD0104)

    samples = [(_clamp16(rnd.gauss(0, 8000)), _clamp16(rnd.gauss(0, 8000))) for _ in range(80)]
    await run_scenario(dut, samples, locked=False)


@cocotb.test()
async def nonzero_initial_offset(dut):
    """A nonzero, negative-signed initial_offset_q15 exercises the
    Q1.15->Q0.16 reset-time conversion's wraparound case."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x9FF5E7)

    samples = [(_clamp16(rnd.gauss(0, 8000)), _clamp16(rnd.gauss(0, 8000))) for _ in range(80)]
    # -8000 in Q1.15 (~ -0.244), well clear of both 0 and the [0,0.5)/
    # [0.5,1) wrap boundary.
    await run_scenario(dut, samples, initial_offset_q15=-8000, locked=True)
