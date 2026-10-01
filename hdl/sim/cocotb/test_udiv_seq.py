"""cocotb testbench for hdl/rtl/common/udiv_seq.v (built at WIDTH=16).

Reference: Python integer floor division, independent of the RTL's
restoring algorithm. Contract checked on every division:
  - start is sampled on an edge where busy is low; done pulses exactly
    WIDTH+1 cycles later with quotient valid;
  - busy is high from that edge until done; operands may change after
    the start edge without effect;
  - quotient holds until the next done, including while busy;
  - divisor 0 yields an all-ones quotient (documented, caller-guarded).
Also covers back-to-back starts (start during the done cycle) and a reset
mid-division.

Timing idiom: outputs are sampled at ReadOnly() after a rising edge;
inputs are driven 1 ns later (inside the same 10 ns cycle), so they are
stable well before the next edge.
"""

import random

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ReadOnly, RisingEdge, Timer

WIDTH = 16
MASK = (1 << WIDTH) - 1


async def edge(dut):
    """Next rising edge, with that edge's register updates settled."""
    await RisingEdge(dut.clk)
    await ReadOnly()


async def drive_point():
    """Leave the ReadOnly phase so inputs can be written for the next edge."""
    await Timer(1, "ns")


def expected(a, b):
    return MASK if b == 0 else a // b


async def start_clock_and_reset(dut):
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    dut.rst.value = 1
    dut.start.value = 0
    dut.dividend.value = 0
    dut.divisor.value = 0
    for _ in range(2):
        await edge(dut)
    await drive_point()
    dut.rst.value = 0
    await edge(dut)
    assert dut.busy.value == 0 and dut.done.value == 0


async def divide(dut, a, b):
    """Call at a drive point with busy low. Returns at the ReadOnly point
    of the done cycle, having checked latency, busy and the quotient."""
    held = int(dut.quotient.value)
    dut.start.value = 1
    dut.dividend.value = a
    dut.divisor.value = b
    await edge(dut)  # start sampled on this edge
    await drive_point()
    dut.start.value = 0
    dut.dividend.value = random.getrandbits(WIDTH)  # must be ignored now
    dut.divisor.value = random.getrandbits(WIDTH)
    assert dut.busy.value == 1, f"{a}/{b}: busy low on the start edge"
    for cycle in range(1, WIDTH + 1):
        await edge(dut)
        assert dut.busy.value == 1, f"{a}/{b}: busy low {cycle} cycle(s) after start"
        assert (
            dut.done.value == 0
        ), f"{a}/{b}: done after {cycle} cycle(s), expected {WIDTH + 1}"
        assert (
            int(dut.quotient.value) == held
        ), f"{a}/{b}: previous quotient not held while busy"
    await edge(dut)
    assert dut.done.value == 1, f"{a}/{b}: no done {WIDTH + 1} cycles after start"
    assert dut.busy.value == 0
    got = int(dut.quotient.value)
    assert got == expected(a, b), f"{a}/{b}: quotient {got}, expected {expected(a, b)}"
    return got


@cocotb.test(timeout_time=5, timeout_unit="ms")
async def edge_and_random_operands(dut):
    random.seed(0xD1)
    await start_clock_and_reset(dut)
    cases = [
        (0, 1),
        (1, 1),
        (MASK, 1),
        (MASK, MASK),
        (MASK - 1, MASK),
        (1, MASK),
        (12345, 0),
        (0, 0),
        (1 << (WIDTH - 1), 3),
        (MASK, 2),
    ]
    cases += [
        (random.getrandbits(WIDTH), random.getrandbits(random.randint(1, WIDTH)) or 1)
        for _ in range(300)
    ]
    checks = 0
    for a, b in cases:
        await drive_point()
        await divide(dut, a, b)
        await edge(dut)  # one idle cycle between divisions
        assert dut.done.value == 0, "done held for more than one cycle"
        checks += 1
    assert checks == len(cases) > 0


@cocotb.test(timeout_time=5, timeout_unit="ms")
async def back_to_back_and_hold(dut):
    """Start again during the done cycle (busy is already low there), then
    check the quotient holds through idle cycles."""
    random.seed(0xD2)
    await start_clock_and_reset(dut)
    await drive_point()
    prev = await divide(dut, 50000, 7)
    for _ in range(50):
        await drive_point()  # still the done cycle
        a, b = random.getrandbits(WIDTH), random.getrandbits(8) or 1
        prev_before = int(dut.quotient.value)
        assert prev_before == prev
        prev = await divide(dut, a, b)
    await drive_point()
    for _ in range(10):
        await edge(dut)
        assert int(dut.quotient.value) == prev, "quotient not held while idle"
        assert dut.done.value == 0 and dut.busy.value == 0


@cocotb.test(timeout_time=5, timeout_unit="ms")
async def reset_mid_division(dut):
    random.seed(0xD3)
    await start_clock_and_reset(dut)
    await drive_point()
    dut.start.value = 1
    dut.dividend.value = 40000
    dut.divisor.value = 3
    await edge(dut)
    await drive_point()
    dut.start.value = 0
    for _ in range(5):
        await edge(dut)
    await drive_point()
    dut.rst.value = 1
    await edge(dut)
    assert dut.busy.value == 0 and dut.done.value == 0 and int(dut.quotient.value) == 0
    await drive_point()
    dut.rst.value = 0
    for _ in range(2 * WIDTH):
        await edge(dut)
        assert dut.done.value == 0, "done pulsed for a division aborted by reset"
        assert dut.busy.value == 0
    await drive_point()
    await divide(dut, 40000, 3)  # still works afterwards
