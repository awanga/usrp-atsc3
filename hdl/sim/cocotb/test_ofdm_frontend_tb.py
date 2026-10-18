"""Integration testbench: freq_correction -> cp_removal -> fft_engine ->
pilot_extractor, with frame_sync on the corrected stream
(tb/ofdm_frontend_tb.v).

Reference: the C++ chain itself, every stage linking the real atsc3_lib:
FreqCorrection (freq_correction_gen, which also gives the Q1.31 increment
the bench programs), CpRemoval (cp_removal_gen), FixedPointFftEngine per
symbol (fft_engine_gen), PilotExtractor per symbol, symbol index
counting from 0 (pilot_extractor_gen), and FrameSync fed one symbol
(fft_size + cp_length samples) per process() call (frame_sync_gen). The
second symbol carries the model's preamble reference, pre-rotated by the
CFO the front end removes, so frame_sync leaves SEARCHING there; its
FrameEvents and final state are compared with the model's.

Real clock ratio: the source offers one sample per RATIO = 16 clocks,
6.25 MS/s against the 100 MHz nominal clock (axi4s_types.vh). The source
here can wait when the chain backpressures; a real ADC cannot, so every
cycle a sample was due but not accepted is counted as a source stall.

Checked: every FFT bin on the fft -> pilot link (tap) and every pilot
record bit-exact, TLAST per symbol on both; status counters (samples out
of freq_correction, cp_removal -> fft_engine beats and symbols, FFT
symbols into the pilot extractor) advance by exactly what was sent;
frame_sync's state, frame number, expected boundary, confidence and
sample counter equal the model's after the last symbol and its events
match; no config or framing flags raised. realtime_budget_8k is an expected
failure: freq_correction needs 18 cycles per sample and the sequential
FFT about 190k cycles per 8K symbol, against 16 and 147k available at
this ratio (tracked as throughput work); it starts passing when both
close.

Dev runs two 8K symbols; HDL_FULL=1 (nightly) adds three 16K symbols
with output backpressure.
"""

import cmath
import math
import os
import random

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import FallingEdge, RisingEdge

from test_cp_removal import pack, run_golden as cp_golden, unpack
from test_fft_engine import run_golden as fft_golden
from test_frame_correlator_tb import model_reference
from test_frame_sync import ev_field, run_golden as fs_golden
from test_freq_correction import run_golden as fc_golden
from test_pilot_extractor import run_golden as pe_golden, unpack_pilot

FULL = os.environ.get("HDL_FULL") == "1"
RATIO = 16
CFO_HZ = 12345.6


def cfo_increment():
    return fc_golden([("F", CFO_HZ)])[0][1]


def with_preamble(samples, symbol, symbol_len, inc):
    """The model's preamble reference (scaled to half amplitude), rotated by
    the CFO that freq_correction removes, replacing one whole symbol."""
    ref = model_reference()
    theta = -math.pi * inc / 2**31  # radians per sample
    out = list(samples)
    for k in range(symbol * symbol_len, (symbol + 1) * symbol_len):
        r = ref[(k - symbol * symbol_len) % 13]
        v = complex(r[0], r[1]) * 0.5 * cmath.exp(1j * theta * k)
        out[k] = (round(v.real), round(v.imag))
    return out


def golden_chain(fft_size, cp_num, pattern, samples, inc):
    """Returns (FFT bins as (re, im, last), pilot records as (record,
    last), frame_sync results per symbol as (events, state))."""
    script = fc_golden([("F", CFO_HZ)] + [("S", re, im, False) for re, im in samples])
    assert script[0][1] == inc
    corrected = [ev[4] for ev in script[1:]]
    cp_len = cp_num * (fft_size // 8192)
    sym_len = fft_size + cp_len
    fs_script = [("P", (fft_size, cp_len, 1, 1, 22937, 3, 5, 10, 64))]
    for i in range(len(corrected) // sym_len):
        fs_script.append(("K", corrected[i * sym_len : (i + 1) * sym_len]))
    fs_results = fs_golden(fs_script)[1:]

    bins = []
    symbols = []
    for sym in cp_golden(fft_size, cp_num, corrected):
        assert len(sym) == fft_size
        res = fft_golden(fft_size, sym)
        bins += [(re, im, k == fft_size - 1) for k, (re, im) in enumerate(res)]
        symbols.append((len(symbols), res))

    pilots = []
    for recs in pe_golden(fft_size, pattern, 0, symbols):
        pilots += [(r, i == len(recs) - 1) for i, r in enumerate(recs)]
    return bins, pilots, fs_results


async def reset(dut, inc, fft_size, cp_num, pattern):
    cp_len = cp_num * (fft_size // 8192)
    dut.cfg_cp_length.value = cp_len
    dut.cfg_chunk_len.value = fft_size + cp_len
    dut.fs_ev_tready.value = 1
    dut.rst.value = 1
    dut.cfg_phase_inc.value = inc & 0xFFFFFFFF
    dut.cfg_fft_size.value = fft_size
    dut.cfg_cp_fraction_numerator.value = cp_num
    dut.cfg_pilot_pattern.value = pattern
    dut.s_axis_tvalid.value = 0
    dut.s_axis_tdata.value = 0
    dut.s_axis_tlast.value = 0
    dut.m_axis_tready.value = 1
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst.value = 0
    await RisingEdge(dut.clk)


async def source(dut, samples, stalls):
    """One sample due every RATIO cycles; waits (counting stall cycles)
    when the chain is not ready."""
    for re, im in samples:
        dut.s_axis_tvalid.value = 1
        dut.s_axis_tdata.value = pack(re, im)
        waited = 0
        while True:
            await RisingEdge(dut.clk)
            if dut.s_axis_tready.value == 1:
                break
            waited += 1
        stalls[0] += waited
        dut.s_axis_tvalid.value = 0
        for _ in range(max(0, RATIO - 1 - waited)):
            await RisingEdge(dut.clk)


async def fft_tap(dut, expected):
    idx = 0
    while idx < len(expected):
        await RisingEdge(dut.clk)
        if dut.fft_fire.value == 1:
            got = unpack(int(dut.fft_tdata.value))
            re, im, last = expected[idx]
            assert got == (re, im), f"FFT bin {idx}: expected {(re, im)}, got {got}"
            assert int(dut.fft_tlast.value) == int(last), f"FFT bin {idx}: TLAST"
            idx += 1


async def sink(dut, expected, rnd):
    idx = 0
    while idx < len(expected):
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value == 1 and dut.m_axis_tready.value == 1:
            got = unpack_pilot(dut.m_axis_tdata.value)
            rec, last = expected[idx]
            assert got == rec, f"pilot {idx}: expected {rec}, got {got}"
            assert int(dut.m_axis_tlast.value) == int(last), f"pilot {idx}: TLAST"
            idx += 1
        if rnd is not None:
            dut.m_axis_tready.value = int(rnd.random() < 0.7)


async def fs_event_sink(dut, events):
    """frame_sync's event output is always ready, so every valid cycle is a
    transfer."""
    while True:
        await FallingEdge(dut.clk)
        if dut.fs_ev_tvalid.value == 1:
            assert dut.fs_ev_tlast.value == 1, "frame_sync event TLAST"
            events.append(int(dut.fs_ev_tdata.value))


async def run_chain(
    dut, fft_size, cp_num, pattern, n_symbols, seed, stall_output=False
):
    rnd = random.Random(seed)
    cp_len = cp_num * (fft_size // 8192)
    samples = [
        (rnd.randint(-8000, 8000), rnd.randint(-8000, 8000))
        for _ in range(n_symbols * (fft_size + cp_len) + 5)
    ]  # tail: next CP
    inc = cfo_increment()
    if n_symbols >= 2:
        samples = with_preamble(samples, 1, fft_size + cp_len, inc)
    bins, pilots, fs_results = golden_chain(fft_size, cp_num, pattern, samples, inc)
    assert len(bins) == n_symbols * fft_size
    if n_symbols >= 2:
        assert (
            fs_results[1][1][0] == 1
        ), "stimulus does not leave SEARCHING in the model"

    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    await reset(dut, inc, fft_size, cp_num, pattern)
    counters = ("fc_out_beats", "cp_out_beats", "cp_out_symbols", "fft_out_symbols")
    before = {c: int(getattr(dut, c).value) for c in counters}
    stalls = [0]
    fs_events = []
    cocotb.start_soon(fs_event_sink(dut, fs_events))
    src = cocotb.start_soon(source(dut, samples, stalls))
    tap = cocotb.start_soon(fft_tap(dut, bins))
    await sink(dut, pilots, rnd if stall_output else None)
    await tap
    await src
    for _ in range(40):  # the tail samples clear freq_correction
        await RisingEdge(dut.clk)

    delta = {c: int(getattr(dut, c).value) - before[c] for c in counters}
    assert delta == {
        "fc_out_beats": len(samples),
        "cp_out_beats": n_symbols * fft_size,
        "cp_out_symbols": n_symbols,
        "fft_out_symbols": n_symbols,
    }, f"status counters: {delta}"
    for _ in range(100):
        await FallingEdge(dut.clk)
        if dut.fs_mon_busy.value == 0:
            break
    assert dut.fs_mon_busy.value == 0, "frame_sync still busy"
    want_events = [e for events, _ in fs_results for e in events]
    got_events = [
        (
            ev_field(w, "type"),
            ev_field(w, "sample_index"),
            ev_field(w, "frame"),
            ev_field(w, "subframe"),
            ev_field(w, "symbol"),
            ev_field(w, "confidence"),
        )
        for w in fs_events
    ]
    assert (
        got_events == want_events
    ), f"frame_sync events {got_events}, model {want_events}"
    state, frame, expected, conf, _ = fs_results[-1][1]
    got = (
        int(dut.fs_mon_state.value),
        int(dut.fs_mon_frame_number.value),
        int(dut.fs_mon_expected_boundary.value),
        int(dut.fs_mon_confidence.value),
    )
    assert got == (
        state,
        frame,
        expected,
        conf,
    ), f"frame_sync {got}, model {fs_results[-1][1]}"
    assert int(dut.fs_mon_sample_counter.value) == n_symbols * (fft_size + cp_len)
    flags = (
        "cp_fft_size_invalid",
        "cp_numerator_clamped",
        "fft_size_invalid",
        "pe_fft_size_invalid",
        "pe_pattern_invalid",
        "pe_num_active_invalid",
        "pe_framing_error",
        "fs_cfg_invalid",
        "fs_chunk_overflow",
    )
    raised = [f for f in flags if int(getattr(dut, f).value)]
    assert not raised, f"flags raised: {raised}"
    dut._log.info(
        "%d symbols of %d: %d pilots, source stalled %d cycles",
        n_symbols,
        fft_size,
        len(pilots),
        stalls[0],
    )
    return stalls[0]


@cocotb.test(timeout_time=300, timeout_unit="ms")
async def two_symbols_8k(dut):
    """Two 8K symbols (CP 1024/8192, PP3) through the chain at the real
    ratio, with a 12.3 kHz CFO correction."""
    await run_chain(dut, 8192, 1024, 3, 2, 0x0F01)


@cocotb.test(timeout_time=300, timeout_unit="ms", expect_fail=True)
async def realtime_budget_8k(dut):
    """At 16 clocks/sample the chain must never stall the source. Known to
    fail today (freq_correction 18 cycles/sample, sequential FFT ~190k
    cycles/symbol vs 147k available)."""
    stalls = await run_chain(dut, 8192, 1024, 3, 2, 0x0F02)
    assert stalls == 0, f"source stalled {stalls} cycles: chain slower than real time"


@cocotb.test(timeout_time=1500, timeout_unit="ms", skip=not FULL)
async def three_symbols_16k_with_stalls(dut):
    """Nightly: three 16K symbols (PP7) with random output backpressure."""
    await run_chain(dut, 16384, 2048, 7, 3, 0x0F03, stall_output=True)
