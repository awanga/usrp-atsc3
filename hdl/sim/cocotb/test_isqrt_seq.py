"""cocotb testbench for hdl/rtl/common/isqrt_seq.v (built at IN_W=48).

Reference: Python math.isqrt, independent of the RTL's digit recurrence.
Contract checked on every root:
  - start is sampled on an edge where busy is low; done pulses exactly
    IN_W/2+1 cycles later with root valid;
  - busy is high from that edge until done; the operand may change after
    the start edge without effect;
  - root holds until the next done, including while busy.
Also covers back-to-back starts (start during the done cycle), start while
busy being ignored, and a reset mid-root. Inputs: 0, 1, every perfect
square and its neighbours at the bit boundaries, the maximum, and random
values over the whole range.

Timing idiom as in test_udiv_seq.py.
"""

import math
import random

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ReadOnly, RisingEdge, Timer

IN_W = 48
LATENCY = IN_W // 2 + 1
MAX_IN = (1 << IN_W) - 1


async def edge(dut):
    await RisingEdge(dut.clk)
    await ReadOnly()


async def drive_point():
    await Timer(1, "ns")


async def start_clock_and_reset(dut):
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    dut.rst.value = 1
    dut.start.value = 0
    dut.x.value = 0
    for _ in range(2):
        await edge(dut)
    await drive_point()
    dut.rst.value = 0
    await edge(dut)
    assert dut.busy.value == 0 and dut.done.value == 0
    await drive_point()


async def isqrt(dut, x):
    """Call at a drive point with busy low. Returns at the ReadOnly point of
    the done cycle, having checked latency, busy and root hold."""
    held = int(dut.root.value)
    dut.start.value = 1
    dut.x.value = x
    await edge(dut)
    await drive_point()
    dut.start.value = 1 if random.random() < 0.3 else 0  # ignored while busy
    dut.x.value = random.getrandbits(IN_W)
    assert dut.busy.value == 1, f"isqrt({x}): busy low on the start edge"
    for cycle in range(1, LATENCY):
        await edge(dut)
        assert dut.busy.value == 1, f"isqrt({x}): busy low {cycle} cycle(s) after start"
        assert dut.done.value == 0, f"isqrt({x}): done after {cycle}, want {LATENCY}"
        assert int(dut.root.value) == held, f"isqrt({x}): root not held while busy"
        await drive_point()
    await edge(dut)
    assert dut.done.value == 1, f"isqrt({x}): no done {LATENCY} cycles after start"
    assert dut.busy.value == 0, f"isqrt({x}): busy still high on done"
    got = int(dut.root.value)
    assert got == math.isqrt(x), f"isqrt({x}): expected {math.isqrt(x)}, got {got}"
    await drive_point()
    dut.start.value = 0
    await edge(dut)
    assert dut.done.value == 0, f"isqrt({x}): done lasted more than one cycle"
    assert int(dut.root.value) == got, f"isqrt({x}): root not held after done"
    await drive_point()


@cocotb.test()
async def edges_and_squares(dut):
    """Boundaries of every bit pair, perfect squares and their neighbours."""
    await start_clock_and_reset(dut)
    values = {0, 1, 2, 3, 4, MAX_IN, MAX_IN - 1}
    for k in range(IN_W + 1):
        for d in (-1, 0, 1):
            values.add(max(0, min(MAX_IN, (1 << k) + d)))
    for r in (2, 3, 255, 256, 65535, 65536, (1 << 24) - 1):
        for d in (-1, 0, 1):
            values.add(max(0, min(MAX_IN, r * r + d)))
    for x in sorted(values):
        await isqrt(dut, x)
    dut._log.info("%d edge values", len(values))


@cocotb.test()
async def random_values(dut):
    """Random operands over the whole range, and of every magnitude."""
    await start_clock_and_reset(dut)
    rnd = random.Random(0x15127)
    random.seed(0x15127)
    for _ in range(300):
        await isqrt(dut, rnd.getrandbits(rnd.randint(1, IN_W)))


@cocotb.test()
async def back_to_back_and_reset(dut):
    """A start in the done cycle begins the next root; reset abandons one."""
    await start_clock_and_reset(dut)
    xs = [10**12, 99, MAX_IN]
    dut.start.value = 1
    dut.x.value = xs[0]
    await edge(dut)  # first start sampled
    for prev, nxt in zip(xs, xs[1:]):
        await drive_point()
        dut.start.value = 0
        for _ in range(LATENCY):
            await edge(dut)
        assert dut.done.value == 1, "no done at the expected cycle"
        assert int(dut.root.value) == math.isqrt(prev)
        await drive_point()
        dut.x.value = nxt
        dut.start.value = 1  # sampled on the edge after done: busy is low
        await edge(dut)
        assert dut.busy.value == 1, "back-to-back start was not taken"
    await drive_point()
    dut.start.value = 0
    for _ in range(LATENCY):
        await edge(dut)
    assert int(dut.root.value) == math.isqrt(xs[-1])

    await drive_point()
    dut.start.value = 1
    dut.x.value = 1 << 40
    await edge(dut)
    await drive_point()
    dut.start.value = 0
    for _ in range(5):
        await edge(dut)
    await drive_point()
    dut.rst.value = 1
    await edge(dut)
    await drive_point()
    dut.rst.value = 0
    await edge(dut)
    assert dut.busy.value == 0 and dut.done.value == 0 and int(dut.root.value) == 0
    for _ in range(LATENCY + 2):
        await edge(dut)
        assert dut.done.value == 0, "abandoned root still completed after reset"
