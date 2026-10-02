"""Integration testbench: freq_correction -> cp_removal -> fft_engine ->
pilot_extractor (tb/ofdm_frontend_tb.v).

Reference: the C++ chain itself, every stage linking the real atsc3_lib:
FreqCorrection (freq_correction_gen, which also gives the Q1.31 increment
the bench programs), CpRemoval (cp_removal_gen), FixedPointFftEngine per
symbol (fft_engine_gen) and PilotExtractor per symbol, symbol index
counting from 0 (pilot_extractor_gen).

Real clock ratio: the source offers one sample per RATIO = 16 clocks,
6.25 MS/s against the 100 MHz nominal clock (axi4s_types.vh). The source
here can wait when the chain backpressures; a real ADC cannot, so every
cycle a sample was due but not accepted is counted as a source stall.

Checked: every FFT bin on the fft -> pilot link (tap) and every pilot
record bit-exact, TLAST per symbol on both; status counters (samples out
of freq_correction, cp_removal -> fft_engine beats and symbols, FFT
symbols into the pilot extractor) advance by exactly what was sent; no
config or framing flags raised. realtime_budget_8k is an expected
failure: freq_correction needs 18 cycles per sample and the sequential
FFT about 190k cycles per 8K symbol, against 16 and 147k available at
this ratio (tracked as throughput work); it starts passing when both
close.

Dev runs two 8K symbols; HDL_FULL=1 (nightly) adds three 16K symbols
with output backpressure.
"""

import os
import random

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge

from test_cp_removal import pack, run_golden as cp_golden, unpack
from test_fft_engine import run_golden as fft_golden
from test_freq_correction import run_golden as fc_golden
from test_pilot_extractor import run_golden as pe_golden, unpack_pilot

FULL = os.environ.get("HDL_FULL") == "1"
RATIO = 16
CFO_HZ = 12345.6


def golden_chain(fft_size, cp_num, pattern, samples):
    """Returns (phase increment, FFT bins as (re, im, last), pilot records
    as (record, last))."""
    script = fc_golden([("F", CFO_HZ)] + [("S", re, im, False) for re, im in samples])
    inc = script[0][1]
    corrected = [ev[4] for ev in script[1:]]

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
    return inc, bins, pilots


async def reset(dut, inc, fft_size, cp_num, pattern):
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


async def run_chain(
    dut, fft_size, cp_num, pattern, n_symbols, seed, stall_output=False
):
    rnd = random.Random(seed)
    cp_len = cp_num * (fft_size // 8192)
    samples = [
        (rnd.randint(-8000, 8000), rnd.randint(-8000, 8000))
        for _ in range(n_symbols * (fft_size + cp_len) + 5)
    ]  # tail: next CP
    inc, bins, pilots = golden_chain(fft_size, cp_num, pattern, samples)
    assert len(bins) == n_symbols * fft_size

    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    await reset(dut, inc, fft_size, cp_num, pattern)
    counters = ("fc_out_beats", "cp_out_beats", "cp_out_symbols", "fft_out_symbols")
    before = {c: int(getattr(dut, c).value) for c in counters}
    stalls = [0]
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
    flags = (
        "cp_fft_size_invalid",
        "cp_numerator_clamped",
        "fft_size_invalid",
        "pe_fft_size_invalid",
        "pe_pattern_invalid",
        "pe_num_active_invalid",
        "pe_framing_error",
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
