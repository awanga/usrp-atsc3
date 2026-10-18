"""cocotb testbench for hdl/rtl/sync/frame_correlator.v
(tb/frame_correlator_tb.v: the correlator and the RAM it reads).

Bit-exact comparison against the real golden model
(FrameSync::compute_correlation(), ATSC3_FIXED_POINT=ON) via the
hdl/sim/golden/frame_sync_gen CLI: the Q1.15 ratio for one chunk, one
offset. Stimuli cover the preamble reference itself at every phase and
amplitude, partial copies, reference plus noise, noise at several levels,
rail values (-32768 and 32767 on both rails), DC and silence, over
correlation lengths from 4 to 1000 and random offsets. Timing: done comes
after at least len cycles and within the datapath's worst case; start while
busy is ignored; a reset mid-correlation abandons it. The reference table
(frame_sync_ref.vh) is checked against the model's own preamble.

Run via test_runner.py (pytest), not directly.
"""

import math
import random
import re
import subprocess
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import FallingEdge, RisingEdge

REPO_ROOT = Path(__file__).resolve().parents[3]
FRAME_SYNC_GEN = REPO_ROOT / "build-fxp" / "hdl" / "sim" / "golden" / "frame_sync_gen"
REF_VH = REPO_ROOT / "hdl" / "rtl" / "sync" / "frame_sync_ref.vh"

ADDR_W = 12
DEFAULT_CFG = (8192, 1024, 1, 1, 22937, 3, 5, 10, 64)  # P-line defaults


def pack(re_, im):
    return ((re_ & 0xFFFF) << 16) | (im & 0xFFFF)


def gen(lines):
    if not FRAME_SYNC_GEN.exists():
        raise FileNotFoundError(
            f"{FRAME_SYNC_GEN} not found -- build it first: "
            "cmake --build build-fxp --target frame_sync_gen "
            "(requires -DATSC3_FIXED_POINT=ON -DATSC3_ENABLE_HDL_STUBS=ON)"
        )
    proc = subprocess.run(
        [str(FRAME_SYNC_GEN)],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        check=True,
    )
    return proc.stdout.splitlines()


def model_reference():
    """The 13-entry reference period, (re, im) each, from the model."""
    out = gen(["Z"])
    ref = {}
    for line in out:
        _, k, re_, im = line.split()
        ref[int(k)] = (int(re_), int(im))
    assert sorted(ref) == list(range(13))
    return [ref[k] for k in range(13)]


def clamp16(v):
    return max(-32768, min(32767, int(round(v))))


def make_signal(rnd, kind, n, ref):
    """n samples of the given kind, as (re, im) int16 pairs."""
    shift = rnd.randrange(13)
    if kind == "ref":
        a = rnd.choice([0.02, 0.1, 0.5, 1.0, 1.5, 1.99])
        return [
            (
                clamp16(a * ref[(i + shift) % 13][0]),
                clamp16(a * ref[(i + shift) % 13][1]),
            )
            for i in range(n)
        ]
    if kind == "partial":
        m = rnd.randint(1, max(1, n - 1))
        return [ref[(i + shift) % 13] if i < m else (0, 0) for i in range(n)]
    if kind == "delayed":
        d = rnd.randint(1, max(1, n - 8))
        return [(0, 0) if i < d else ref[(i - d + shift) % 13] for i in range(n)]
    if kind == "ref_noise":
        sigma = rnd.choice([30, 500, 4000, 12000])
        return [
            (
                clamp16(ref[(i + shift) % 13][0] + rnd.gauss(0, sigma)),
                clamp16(ref[(i + shift) % 13][1] + rnd.gauss(0, sigma)),
            )
            for i in range(n)
        ]
    if kind == "noise_small":
        return [(rnd.randint(-40, 40), rnd.randint(-40, 40)) for _ in range(n)]
    if kind == "noise_full":
        return [
            (rnd.randint(-32768, 32767), rnd.randint(-32768, 32767)) for _ in range(n)
        ]
    if kind == "rails":
        return [
            (rnd.choice([-32768, 32767]), rnd.choice([-32768, 32767])) for _ in range(n)
        ]
    if kind == "dc":
        v = (rnd.randint(-32768, 32767), rnd.randint(-32768, 32767))
        return [v] * n
    if kind == "tiny":
        return [(rnd.randint(-1, 1), rnd.randint(-1, 1)) for _ in range(n)]
    assert kind == "zero"
    return [(0, 0)] * n


KINDS = [
    "ref",
    "delayed",
    "partial",
    "ref_noise",
    "noise_small",
    "noise_full",
    "rails",
    "dc",
    "tiny",
    "zero",
]


def golden_ratios(fft, cases):
    """cases: list of (samples, offset). Returns the model's Q1.15 ratios."""
    cfg = list(DEFAULT_CFG)
    cfg[0] = fft
    lines = ["P " + " ".join(str(v) for v in cfg)]
    for samples, offset in cases:
        lines.append(f"Q {len(samples)} {offset}")
        lines += [f"{re_} {im}" for re_, im in samples]
    out = gen(lines)
    assert len(out) == len(cases)
    return [int(line.split()[1]) for line in out]


async def reset(dut):
    dut.rst.value = 1
    dut.wr_en.value = 0
    dut.wr_addr.value = 0
    dut.wr_data.value = 0
    dut.start.value = 0
    dut.base.value = 0
    dut.len.value = 4
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst.value = 0
    await RisingEdge(dut.clk)


async def fill(dut, samples):
    for i, (re_, im) in enumerate(samples):
        dut.wr_en.value = 1
        dut.wr_addr.value = i
        dut.wr_data.value = pack(re_, im)
        await RisingEdge(dut.clk)
    dut.wr_en.value = 0


async def correlate(dut, base, length, max_cycles=None):
    """Pulses start and waits for done. Returns (ratio, cycles from the
    start edge to the done cycle)."""
    await FallingEdge(dut.clk)
    dut.base.value = base
    dut.len.value = length
    dut.start.value = 1
    await RisingEdge(dut.clk)  # start sampled
    await FallingEdge(dut.clk)
    dut.start.value = 0
    dut.base.value = 0  # must be latched by now
    dut.len.value = 4
    cycles = 0
    limit = max_cycles or (length + 400)
    while True:
        await RisingEdge(dut.clk)
        cycles += 1
        await FallingEdge(dut.clk)
        if dut.done.value == 1:
            assert dut.busy.value == 0, "busy still high with done"
            return int(dut.ratio.value), cycles
        assert dut.busy.value == 1, "busy dropped before done"
        assert cycles < limit, f"no done within {limit} cycles"


async def run_cases(dut, fft, cases, golden):
    for (samples, offset), want in zip(cases, golden):
        await fill(dut, samples)
        got, cycles = await correlate(dut, offset, fft)
        assert got == want, f"len {fft} offset {offset}: expected {want}, got {got}"
        assert (
            cycles >= fft
        ), f"done after {cycles} cycles, fewer than the {fft} samples"


@cocotb.test(timeout_time=2, timeout_unit="sec")
async def reference_table_matches_model(dut):
    """frame_sync_ref.vh holds exactly the model's 13 reference samples."""
    ref = model_reference()
    entries = re.findall(
        r"4'd(\d+): frame_ref = \{(-?)16'sd(\d+), (-?)16'sd(\d+)\};", REF_VH.read_text()
    )
    assert len(entries) == 13, f"{len(entries)} table entries"
    for k, sre, vre, sim, vim in entries:
        got = (-int(vre) if sre else int(vre), -int(vim) if sim else int(vim))
        assert got == ref[int(k)], f"entry {k}: table {got}, model {ref[int(k)]}"
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())


@cocotb.test(timeout_time=3000, timeout_unit="ms")
async def ratios_match_model(dut):
    """Every signal kind at lengths 4..1000 and random offsets."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    await reset(dut)
    rnd = random.Random(0xC0221)
    ref = model_reference()
    lengths = [4, 5, 13, 14, 26, 27, 64, 100, 255, 1000]
    total = 0
    seen_nonzero = 0
    for fft in lengths:
        cases = []
        for kind in KINDS:
            n = fft + rnd.randint(0, 40)
            offset = rnd.randint(0, n - fft)
            cases.append((make_signal(rnd, kind, n, ref), offset))
        golden = golden_ratios(fft, cases)
        seen_nonzero += sum(1 for g in golden if g > 0)
        await run_cases(dut, fft, cases, golden)
        total += len(cases)
    assert seen_nonzero > total // 2, "stimulus mostly correlates to zero"
    dut._log.info("%d correlations, %d nonzero", total, seen_nonzero)


@cocotb.test(timeout_time=3000, timeout_unit="ms")
async def reference_peaks_and_quantization(dut):
    """The reference itself correlates at ~1.0 at every phase, and the model
    resolves the small differences between phases (CORDIC error), so the
    exact Q1.15 ratio is compared, not only its neighbourhood."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    await reset(dut)
    ref = model_reference()
    fft = 52
    cases = []
    for shift in range(13):
        sig = [ref[(i + shift) % 13] for i in range(fft)]
        cases.append((sig, 0))
    golden = golden_ratios(fft, cases)
    assert all(32700 <= g <= 33100 for g in golden), golden  # ~1.0 (independent)
    assert len(set(golden)) > 1, "phases indistinguishable: weak stimulus"
    await run_cases(dut, fft, cases, golden)


@cocotb.test(timeout_time=2000, timeout_unit="ms")
async def start_ignored_while_busy_and_reset(dut):
    """A start during a correlation does nothing; reset abandons one."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    await reset(dut)
    rnd = random.Random(7)
    ref = model_reference()
    fft = 40
    sig = make_signal(rnd, "ref_noise", fft, ref)
    want = golden_ratios(fft, [(sig, 0)])[0]
    await fill(dut, sig)
    await FallingEdge(dut.clk)
    dut.base.value = 0
    dut.len.value = fft
    dut.start.value = 1
    await RisingEdge(dut.clk)
    # start stays high with different operands for the whole run
    await FallingEdge(dut.clk)
    dut.base.value = 3
    dut.len.value = 5
    done_seen = 0
    for _ in range(fft + 400):
        await RisingEdge(dut.clk)
        await FallingEdge(dut.clk)
        if dut.done.value == 1:
            done_seen += 1
            assert int(dut.ratio.value) == want, "operands changed mid-run took effect"
            dut.start.value = 0
    assert done_seen >= 1

    # Reset mid-correlation: no done, ratio cleared, ready for the next one.
    dut.start.value = 0
    await FallingEdge(dut.clk)
    dut.base.value = 0
    dut.len.value = fft
    dut.start.value = 1
    await RisingEdge(dut.clk)
    await FallingEdge(dut.clk)
    dut.start.value = 0
    for _ in range(fft // 2):
        await RisingEdge(dut.clk)
    dut.rst.value = 1
    await RisingEdge(dut.clk)
    await FallingEdge(dut.clk)
    dut.rst.value = 0
    assert dut.busy.value == 0 and dut.done.value == 0 and int(dut.ratio.value) == 0
    for _ in range(fft + 400):
        await RisingEdge(dut.clk)
        assert dut.done.value == 0, "abandoned correlation still completed"
    got, _ = await correlate(dut, 0, fft)
    assert got == want
    assert math.isfinite(got)
