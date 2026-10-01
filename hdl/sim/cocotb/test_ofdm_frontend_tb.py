"""Integration testbench: cp_removal feeding fft_engine (tb/ofdm_frontend_tb.v).

Reference: the C++ chain itself -- CpRemoval's symbols (cp_removal_gen)
each transformed by FixedPointFftEngine (fft_engine_gen), both linking the
real atsc3_lib.

Real clock ratio: the source offers one sample per RATIO = 16 clocks,
6.25 MS/s against the 100 MHz nominal clock (axi4s_types.vh). The source
here can wait when the chain backpressures; a real ADC cannot, so every
cycle a sample was due but not accepted is counted as a source stall.

Checked: every output beat bit-exact with TLAST per symbol; status
counters (cp_removal -> fft_engine link beats and symbols) advance by
exactly what was sent; no config flags raised. realtime_budget_8k is an
expected failure: the sequential FFT needs about 190k cycles per 8K
symbol against 147k available at this ratio (tracked as throughput work);
it starts passing when that gap closes.

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

FULL = os.environ.get("HDL_FULL") == "1"
RATIO = 16


def golden_chain(fft_size, cp_num, samples):
    out = []
    for sym in cp_golden(fft_size, cp_num, samples):
        assert len(sym) == fft_size
        res = fft_golden(fft_size, sym)
        out += [(re, im, k == fft_size - 1) for k, (re, im) in enumerate(res)]
    return out


async def reset(dut, fft_size, cp_num):
    dut.rst.value = 1
    dut.cfg_fft_size.value = fft_size
    dut.cfg_cp_fraction_numerator.value = cp_num
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


async def sink(dut, expected, rnd):
    idx = 0
    while idx < len(expected):
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value == 1 and dut.m_axis_tready.value == 1:
            got = unpack(int(dut.m_axis_tdata.value))
            re, im, last = expected[idx]
            assert got == (re, im), f"output {idx}: expected {(re, im)}, got {got}"
            assert int(dut.m_axis_tlast.value) == int(last), f"output {idx}: TLAST"
            idx += 1
        if rnd is not None:
            dut.m_axis_tready.value = int(rnd.random() < 0.7)


async def run_chain(dut, fft_size, cp_num, n_symbols, seed, stall_output=False):
    rnd = random.Random(seed)
    cp_len = cp_num * (fft_size // 8192)
    samples = [
        (rnd.randint(-8000, 8000), rnd.randint(-8000, 8000))
        for _ in range(n_symbols * (fft_size + cp_len) + 5)
    ]  # tail: next CP
    expected = golden_chain(fft_size, cp_num, samples)
    assert len(expected) == n_symbols * fft_size

    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    await reset(dut, fft_size, cp_num)
    beats0, syms0 = int(dut.cp_out_beats.value), int(dut.cp_out_symbols.value)
    stalls = [0]
    src = cocotb.start_soon(source(dut, samples, stalls))
    await sink(dut, expected, rnd if stall_output else None)
    await src

    assert int(dut.cp_out_beats.value) - beats0 == n_symbols * fft_size
    assert int(dut.cp_out_symbols.value) - syms0 == n_symbols
    assert (
        int(dut.cp_fft_size_invalid.value),
        int(dut.cp_numerator_clamped.value),
        int(dut.fft_size_invalid.value),
    ) == (0, 0, 0)
    dut._log.info(
        "%d symbols of %d: source stalled %d cycles", n_symbols, fft_size, stalls[0]
    )
    return stalls[0]


@cocotb.test(timeout_time=200, timeout_unit="ms")
async def two_symbols_8k(dut):
    """Two 8K symbols (CP 1024/8192) through the chain at the real ratio."""
    await run_chain(dut, 8192, 1024, 2, 0x0F01)


@cocotb.test(timeout_time=200, timeout_unit="ms", expect_fail=True)
async def realtime_budget_8k(dut):
    """At 16 clocks/sample the chain must never stall the source. Known to
    fail today (sequential FFT, ~190k cycles/symbol vs 147k available)."""
    stalls = await run_chain(dut, 8192, 1024, 2, 0x0F02)
    assert stalls == 0, f"source stalled {stalls} cycles: chain slower than real time"


@cocotb.test(timeout_time=1000, timeout_unit="ms", skip=not FULL)
async def three_symbols_16k_with_stalls(dut):
    """Nightly: three 16K symbols with random output backpressure."""
    await run_chain(dut, 16384, 2048, 3, 0x0F03, stall_output=True)
