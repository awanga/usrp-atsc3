# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Alfred Wanga
#
"""Integration testbench: freq_correction -> cp_removal -> fft_engine ->
pilot_extractor -> channel_estimator -> equalizer -> constellation_demapper
(tb/ofdm_frontend_tb.v).

Reference: the C++ chain itself, every stage linking the real atsc3_lib:
FreqCorrection (freq_correction_gen, which also gives the Q1.31 increment
the bench programs), CpRemoval (cp_removal_gen), FixedPointFftEngine per
symbol (fft_engine_gen), PilotExtractor per symbol, symbol index
counting from 0 (pilot_extractor_gen), then the channel estimator,
equalizer and demapper CLIs. (FrameSync has no sample path any more: it
tracks validated bootstraps, so it is not part of this chain.)

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
from cocotb.triggers import FallingEdge, RisingEdge

from test_cp_removal import pack, run_golden as cp_golden, unpack
from test_fft_engine import run_golden as fft_golden
from test_freq_correction import run_golden as fc_golden
from test_pilot_extractor import run_golden as pe_golden, unpack_pilot
from test_channel_estimator import run_golden as ce_golden, s16
from test_equalizer import golden as eq_golden
from test_constellation_demapper import golden as dm_golden

FULL = os.environ.get("HDL_FULL") == "1"
RATIO = 16
CFO_HZ = 12345.6


def cfo_increment():
    return fc_golden([("F", CFO_HZ)])[0][1]


# A/322 Table 8.9 length per Table 9.13 code used here
GI_SAMPLES = {5: 1024, 11: 4096}


def golden_chain(fft_size, gi_code, pattern, samples, inc):
    """Returns (FFT bins as (re, im, last), pilot records as (record,
    last), (estimates, equalized cells, LLRs))."""
    script = fc_golden([("F", CFO_HZ)] + [("S", re, im, False) for re, im in samples])
    assert script[0][1] == inc
    corrected = [ev[4] for ev in script[1:]]

    bins = []
    symbols = []
    for sym in cp_golden(fft_size, gi_code, corrected):
        assert len(sym) == fft_size
        res = fft_golden(fft_size, sym)
        bins += [(re, im, k == fft_size - 1) for k, (re, im) in enumerate(res)]
        symbols.append((len(symbols), res))

    pilots = []
    ce_input = []
    for recs in pe_golden(fft_size, pattern, 0, symbols):
        pilots += [(r, i == len(recs) - 1) for i, r in enumerate(recs)]
        ce_input.append([(r[0], (r[2], r[3]), (r[4], r[5])) for r in recs])
    estimates = ce_golden(fft_size, 0, False, 0, ce_input)
    noc = {8192: 6913, 16384: 13825, 32768: 27649}[fft_size]
    first = fft_size // 2 - (noc - 1) // 2  # A/322 Section 8.3
    carriers = []
    for s, est in enumerate(estimates):
        y = bins[s * fft_size + first : s * fft_size + first + noc]
        carriers.append([(re, im, h[0], h[1]) for (re, im, _), h in zip(y, est)])
    (min_q, noise_q), eq_flat = eq_golden(
        0, "0.01", "0.01", [c for sym in carriers for c in sym]
    )
    assert (min_q, noise_q) == (
        107374,
        10737418,
    ), "bench equalizer constants out of date"
    equalized = [eq_flat[i * noc : (i + 1) * noc] for i in range(len(estimates))]
    (dm_bps, dm_scale, dm_norm), llr_flat = dm_golden(
        2, 0, "0.1", 127, [c for sym in equalized for c in sym]
    )
    assert (dm_bps, dm_scale, dm_norm) == (
        6,
        745126,
        4447,
    ), "bench demapper constants stale"
    llrs = [llr_flat[i * noc : (i + 1) * noc] for i in range(len(estimates))]
    return bins, pilots, (estimates, equalized, llrs)


async def reset(dut, inc, fft_size, gi_code, pattern):
    dut.rst.value = 1
    dut.cfg_phase_inc.value = inc & 0xFFFFFFFF
    dut.cfg_fft_size.value = fft_size
    dut.cfg_guard_interval.value = gi_code
    dut.cfg_pilot_pattern.value = pattern
    dut.s_axis_tvalid.value = 0
    dut.s_axis_tdata.value = 0
    dut.s_axis_tlast.value = 0
    dut.m_axis_tready.value = 1
    dut.eq_tready.value = 1
    dut.dm_tready.value = 1
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


async def ce_sink(dut, estimates):
    """channel_estimator output: every carrier of every symbol against the
    model's estimate() run on the extractor's records, TLAST on the last."""
    for s, sym in enumerate(estimates):
        k = 0
        while k < len(sym):
            await RisingEdge(dut.clk)
            if dut.ce_fire.value == 1:
                word = int(dut.ce_tdata.value)
                got = (s16(word >> 16), s16(word))
                assert (
                    got == sym[k]
                ), f"estimate symbol {s} carrier {k}: expected {sym[k]}, got {got}"
                assert int(dut.ce_tlast.value) == int(
                    k == len(sym) - 1
                ), "estimate TLAST"
                k += 1
    assert dut.ce_span_error.value == 0 and dut.ce_order_error.value == 0


async def eq_sink(dut, equalized, rnd):
    """Equalizer output: every carrier of every symbol against the model's
    equalize_sample() on the received bins and the estimates."""
    for s, sym in enumerate(equalized):
        k = 0
        while k < len(sym):
            await FallingEdge(dut.clk)  # mid-cycle: registered outputs are stable
            if dut.eq_tvalid.value == 1 and dut.eq_tready.value == 1:
                word = int(dut.eq_tdata.value)
                got = (s16(word >> 16), s16(word))
                assert (
                    got == sym[k]
                ), f"equalized symbol {s} carrier {k}: expected {sym[k]}, got {got}"
                assert int(dut.eq_tlast.value) == int(
                    k == len(sym) - 1
                ), "equalizer TLAST"
                k += 1
            if rnd is not None:
                dut.eq_tready.value = int(rnd.random() < 0.7)
    assert dut.eq_framing_error.value == 0 and dut.eq_cfg_invalid.value == 0


async def dm_sink(dut, llrs, rnd):
    """Demapper output: every LLR of every cell of every symbol against the
    model's demap_symbol() on the equalized cells, TLAST on the last LLR of
    the symbol's last cell."""
    for s, sym in enumerate(llrs):
        flat = [v for cell in sym for v in cell]
        k = 0
        while k < len(flat):
            await FallingEdge(dut.clk)
            if dut.dm_tvalid.value == 1 and dut.dm_tready.value == 1:
                v = int(dut.dm_tdata.value)
                v = v - 256 if v & 0x80 else v
                assert (
                    v == flat[k]
                ), f"LLR symbol {s} beat {k}: expected {flat[k]}, got {v}"
                assert int(dut.dm_tlast.value) == int(
                    k == len(flat) - 1
                ), "demapper TLAST"
                k += 1
            if rnd is not None:
                dut.dm_tready.value = int(rnd.random() < 0.7)
    assert dut.dm_cfg_invalid.value == 0


async def run_chain(
    dut, fft_size, gi_code, pattern, n_symbols, seed, stall_output=False
):
    rnd = random.Random(seed)
    cp_len = GI_SAMPLES[gi_code]
    samples = [
        (rnd.randint(-8000, 8000), rnd.randint(-8000, 8000))
        for _ in range(n_symbols * (fft_size + cp_len) + 5)
    ]  # tail: next CP
    inc = cfo_increment()
    bins, pilots, (estimates, equalized, llrs) = golden_chain(
        fft_size, gi_code, pattern, samples, inc
    )
    assert len(bins) == n_symbols * fft_size

    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    await reset(dut, inc, fft_size, gi_code, pattern)
    counters = ("fc_out_beats", "cp_out_beats", "cp_out_symbols", "fft_out_symbols")
    before = {c: int(getattr(dut, c).value) for c in counters}
    stalls = [0]
    src = cocotb.start_soon(source(dut, samples, stalls))
    tap = cocotb.start_soon(fft_tap(dut, bins))
    ce = cocotb.start_soon(ce_sink(dut, estimates))
    eqt = cocotb.start_soon(eq_sink(dut, equalized, rnd if stall_output else None))
    dmt = cocotb.start_soon(dm_sink(dut, llrs, rnd if stall_output else None))
    await sink(dut, pilots, rnd if stall_output else None)
    await ce
    await eqt
    await dmt
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
        "cp_gi_invalid",
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
    """Two 8K symbols (GI5_1024, SP6_4) through the chain at the real
    ratio, with a 12.3 kHz CFO correction."""
    await run_chain(dut, 8192, 5, 5, 2, 0x0F01)


@cocotb.test(timeout_time=300, timeout_unit="ms", expect_fail=True)
async def realtime_budget_8k(dut):
    """At 16 clocks/sample the chain must never stall the source. Known to
    fail today (freq_correction 18 cycles/sample, sequential FFT ~190k
    cycles/symbol vs 147k available)."""
    stalls = await run_chain(dut, 8192, 5, 5, 2, 0x0F02)
    assert stalls == 0, f"source stalled {stalls} cycles: chain slower than real time"


@cocotb.test(timeout_time=1500, timeout_unit="ms", skip=not FULL)
async def three_symbols_16k_with_stalls(dut):
    """Nightly: three 16K symbols (GI11_4096, SP24_4) with random output
    backpressure."""
    await run_chain(dut, 16384, 11, 13, 3, 0x0F03, stall_output=True)
