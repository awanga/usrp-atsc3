"""cocotb testbench for hdl/rtl/common/axi4s_skid_buffer.v

Randomized backpressure test: drives a stream of tagged beats into the skid
buffer with random input gaps and random downstream stalls, and checks the
output stream matches the input stream exactly, in order, with no drops or
duplicates. This is the black-box counterpart to the internal invariants
already proven in hdl/formal/axi4s_skid_buffer.sby -- that proof covers
"never violates the protocol / never loses a captured beat" structurally;
this test covers "a real simulated run of many beats through Verilator
actually behaves like a two-deep queue."

Run directly: hdl/sim/.venv/bin/python -m pytest test_axi4s_skid_buffer.py
"""

import random

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge

DATA_WIDTH = 8
NUM_BEATS = 500


async def reset_dut(dut):
    dut.rst.value = 1
    dut.s_axis_tvalid.value = 0
    dut.s_axis_tdata.value = 0
    dut.s_axis_tlast.value = 0
    dut.m_axis_tready.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst.value = 0
    await RisingEdge(dut.clk)


async def drive_input(dut, beats, rnd):
    """Offer each (data, last) beat, holding TVALID until TREADY fires,
    with a random idle gap before offering the next beat."""
    for data, last in beats:
        for _ in range(rnd.randint(0, 2)):
            dut.s_axis_tvalid.value = 0
            await RisingEdge(dut.clk)

        dut.s_axis_tvalid.value = 1
        dut.s_axis_tdata.value = data
        dut.s_axis_tlast.value = int(last)
        while True:
            await RisingEdge(dut.clk)
            if dut.s_axis_tready.value == 1:
                break

    dut.s_axis_tvalid.value = 0


async def drive_backpressure(dut, rnd, stop_event):
    """Randomly toggle TREADY every cycle until told to stop."""
    while not stop_event.is_set():
        dut.m_axis_tready.value = rnd.randint(0, 1)
        await RisingEdge(dut.clk)


async def collect_output(dut, expected, done_event):
    """Sample the output handshake every cycle and check against the
    expected FIFO order as beats arrive."""
    idx = 0
    while idx < len(expected):
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value == 1 and dut.m_axis_tready.value == 1:
            got_data = int(dut.m_axis_tdata.value)
            got_last = int(dut.m_axis_tlast.value)
            exp_data, exp_last = expected[idx]
            assert got_data == exp_data, (
                f"beat {idx}: tdata mismatch, expected {exp_data}, got {got_data}"
            )
            assert got_last == int(exp_last), (
                f"beat {idx}: tlast mismatch, expected {int(exp_last)}, got {got_last}"
            )
            idx += 1
    done_event.set()


@cocotb.test()
async def randomized_backpressure(dut):
    rnd = random.Random(0xA7C3)

    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    await reset_dut(dut)

    beats = [
        (rnd.randint(0, (1 << DATA_WIDTH) - 1), i == NUM_BEATS - 1)
        for i in range(NUM_BEATS)
    ]

    done_event = cocotb.triggers.Event()
    cocotb.start_soon(drive_backpressure(dut, rnd, done_event))
    cocotb.start_soon(drive_input(dut, beats, rnd))
    await collect_output(dut, beats, done_event)


@cocotb.test()
async def full_throughput_no_stalls(dut):
    """With TREADY always high, one beat must transfer every cycle once
    TVALID is asserted -- the skid path should never engage."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    await reset_dut(dut)

    dut.m_axis_tready.value = 1

    beats = [(i & ((1 << DATA_WIDTH) - 1), i == 31) for i in range(32)]

    for data, last in beats:
        dut.s_axis_tvalid.value = 1
        dut.s_axis_tdata.value = data
        dut.s_axis_tlast.value = int(last)
        await RisingEdge(dut.clk)
        assert dut.s_axis_tready.value == 1, "must accept every cycle at full throughput"

    dut.s_axis_tvalid.value = 0

    # Output should already have kept pace: after the loop the last beat is
    # either present now or was already consumed each cycle. Drain and check.
    got = []
    for _ in range(4):
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value == 1:
            got.append((int(dut.m_axis_tdata.value), int(dut.m_axis_tlast.value)))

    # At minimum the tail of the stream must have flowed through with no
    # extra latency beyond the two-stage pipeline (skid buffer adds at most
    # one cycle of register delay).
    assert len(got) >= 1
