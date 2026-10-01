"""cocotb testbench for hdl/rtl/sync/polyphase_fir.v.

Reference: PolyphaseInterpolator::interpolate() itself, through the
timing_recovery_gen CLI's "I" mode (lib/sync/timing_recovery.cc, built
with ATSC3_FIXED_POINT=ON) -- so a coefficient ROM out of step with the
running filter design fails here too.

The testbench owns the 128-entry sample buffer and serves it as the
synchronous-read RAM the module's contract assumes: mem_data reflects the
mem_addr sampled on the previous rising edge.

Checked per call: result bit-exact; done exactly 2*NUM_TAPS+1 cycles
after the start edge; busy high until then; start ignored while busy;
result held until the next done.
"""

import random
import subprocess
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ReadOnly, RisingEdge, Timer

REPO_ROOT = Path(__file__).resolve().parents[3]
GEN = REPO_ROOT / "build-fxp" / "hdl" / "sim" / "golden" / "timing_recovery_gen"
NUM_PHASES = 16
NUM_TAPS = 32
BUF_SIZE = 128
LATENCY = 2 * NUM_TAPS + 1


def run_golden(buf, queries):
    assert GEN.exists(), f"{GEN} missing: cmake --build build-fxp --target timing_recovery_gen"
    lines = ["I", "B " + " ".join(f"{re} {im}" for re, im in buf)]
    lines += [f"Q {p} {b}" for p, b in queries]
    out = subprocess.run([str(GEN)], input="\n".join(lines) + "\n", capture_output=True,
                         text=True, check=True).stdout.split("\n")
    res = [tuple(int(v) for v in ln.split()[1:]) for ln in out if ln.startswith("R ")]
    assert len(res) == len(queries)
    return res


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


async def edge(dut):
    await RisingEdge(dut.clk)
    await ReadOnly()


async def drive_point():
    await Timer(1, "ns")


async def serve_ram(dut, buf):
    """Synchronous-read RAM: data for the address seen at edge N is
    presented after edge N."""
    while True:
        await edge(dut)
        addr = int(dut.mem_addr.value)
        await drive_point()
        re, im = buf[addr]
        dut.mem_data_re.value = re & 0xFFFF
        dut.mem_data_im.value = im & 0xFFFF


async def setup(dut, buf):
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    dut.rst.value = 1
    dut.start.value = 0
    dut.phase.value = 0
    dut.base_idx.value = 0
    dut.mem_data_re.value = 0
    dut.mem_data_im.value = 0
    for _ in range(2):
        await edge(dut)
    await drive_point()
    dut.rst.value = 0
    cocotb.start_soon(serve_ram(dut, buf))
    await edge(dut)
    await drive_point()


async def interpolate(dut, phase, base_idx, poke_start_midway):
    """Call at a drive point with busy low; returns (re, im) at the done
    cycle's ReadOnly point."""
    held = (s16(int(dut.result_re.value)), s16(int(dut.result_im.value)))
    dut.start.value = 1
    dut.phase.value = phase
    dut.base_idx.value = base_idx
    await edge(dut)
    await drive_point()
    dut.start.value = 0
    for cycle in range(1, LATENCY):
        if poke_start_midway and cycle == NUM_TAPS:
            dut.start.value = 1  # must be ignored while busy
            dut.base_idx.value = (base_idx + 17) % BUF_SIZE
        elif poke_start_midway and cycle == NUM_TAPS + 1:
            dut.start.value = 0
        await edge(dut)
        assert dut.busy.value == 1, f"busy low {cycle} cycles after start"
        assert dut.done.value == 0, f"done after {cycle} cycles, expected {LATENCY}"
        got_held = (s16(int(dut.result_re.value)), s16(int(dut.result_im.value)))
        assert got_held == held, "previous result not held while busy"
        await drive_point()
    dut.phase.value = random.randrange(NUM_PHASES)  # operands are sampled at start only
    await edge(dut)
    assert dut.done.value == 1, f"no done {LATENCY} cycles after start"
    assert dut.busy.value == 0
    return s16(int(dut.result_re.value)), s16(int(dut.result_im.value))


async def run_calls(dut, buf, queries, seed):
    random.seed(seed)
    golden = run_golden(buf, queries)
    await setup(dut, buf)
    checks = 0
    for (phase, base_idx), exp in zip(queries, golden):
        got = await interpolate(dut, phase, base_idx, poke_start_midway=random.random() < 0.3)
        assert got == exp, f"phase {phase} base {base_idx}: got {got}, expected {exp}"
        checks += 1
        await drive_point()
    assert checks == len(queries) > 0


@cocotb.test(timeout_time=20, timeout_unit="ms")
async def every_phase_and_wrap(dut):
    """Every phase at base indices that make the 32-tap window wrap the
    ring buffer, on realistic-amplitude random samples."""
    rnd = random.Random(0xF1)
    buf = [(rnd.randint(-16000, 16000), rnd.randint(-16000, 16000)) for _ in range(BUF_SIZE)]
    queries = [(p, b) for p in range(NUM_PHASES) for b in (0, 1, 31, 32, 64, 127)]
    await run_calls(dut, buf, queries, 1)


@cocotb.test(timeout_time=20, timeout_unit="ms")
async def full_scale_saturation(dut):
    """Near-full-scale alternating samples drive the dot product past
    +-1.0, exercising the output saturation and the rounding bias on large
    accumulators."""
    buf = [((32767, -32767) if (i // 3) % 2 else (-32767, 32767)) for i in range(BUF_SIZE)]
    rnd = random.Random(0xF2)
    queries = [(rnd.randrange(NUM_PHASES), rnd.randrange(BUF_SIZE)) for _ in range(40)]
    await run_calls(dut, buf, queries, 2)


@cocotb.test(timeout_time=20, timeout_unit="ms")
async def small_values_rounding(dut):
    """Small sample values, where the round-to-nearest bias on the final
    >>15 decides most outputs."""
    rnd = random.Random(0xF3)
    buf = [(rnd.randint(-3, 3), rnd.randint(-3, 3)) for _ in range(BUF_SIZE)]
    queries = [(rnd.randrange(NUM_PHASES), rnd.randrange(BUF_SIZE)) for _ in range(40)]
    await run_calls(dut, buf, queries, 3)
