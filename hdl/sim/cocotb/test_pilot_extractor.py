"""cocotb testbench for hdl/rtl/ofdm/pilot_extractor.v

Bit-exact comparison against the real golden model
(lib/ofdm/pilot_extractor.cc's PilotExtractor::extract(),
ATSC3_FIXED_POINT=ON) via the hdl/sim/golden/pilot_extractor_gen CLI:
every PilotSymbol field (carrier, type, reference, received value) and
TLAST on each symbol's last pilot.

Covers all eight pilot patterns at 8K, each on the symbol with the
largest scattered offset (symbol Dy-1) and the symbol after it (offset 0,
where the first scattered pilot lands on the edge carrier); 16K and 32K;
active windows of one carrier, every carrier (bin 0 is active, so its
symbol index is used the cycle it arrives) and an odd custom width;
random input gaps and output backpressure with the AXI4-S hold rule
checked; invalid configuration falling back to the C++ defaults with the
flags set; a TLAST in the wrong place setting framing_error; and a reset
mid-symbol. TUSER carries random values off bin 0 to show only bin 0's
is used. The sweeps check that they saw both kinds of type collision
(scattered on a continual carrier, scattered on an edge carrier).

Run via test_runner.py (pytest), not directly.
"""

import os
import random
import subprocess
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge

REPO_ROOT = Path(__file__).resolve().parents[3]
FULL = os.environ.get("HDL_FULL") == "1"
PILOT_EXTRACTOR_GEN = (
    REPO_ROOT / "build-fxp" / "hdl" / "sim" / "golden" / "pilot_extractor_gen"
)
CP_ROM_HEX = REPO_ROOT / "hdl" / "rtl" / "ofdm" / "pilot_cp_positions.hex"

DY = {1: 4, 2: 2, 3: 4, 4: 2, 5: 4, 6: 2, 7: 4, 8: 16}
DEFAULT_ACTIVE = {8192: 6913, 16384: 13825, 32768: 27649}
ROM_BASE = {8192: 0, 16384: 256, 32768: 512}
SCATTERED, CONTINUAL, EDGE = 0, 1, 2


def _to_signed(v, bits):
    v = int(v)
    if v & (1 << (bits - 1)):
        v -= 1 << bits
    return v


def pack(re, im):
    return ((re & 0xFFFF) << 16) | (im & 0xFFFF)


def unpack_pilot(word):
    """status_words.vh PILOT_SYMBOL_* -> (carrier, type, ref_re, ref_im,
    rx_re, rx_im); the reserved byte must be zero."""
    word = int(word)
    assert (word >> 88) & 0xFF == 0, "reserved byte not zero"
    return (
        word & 0xFFFF,
        (word >> 80) & 0xFF,
        _to_signed((word >> 32) & 0xFFFF, 16),
        _to_signed((word >> 16) & 0xFFFF, 16),
        _to_signed((word >> 64) & 0xFFFF, 16),
        _to_signed((word >> 48) & 0xFFFF, 16),
    )


def continual_offsets(fft_size):
    """Relative continual-pilot carriers from the committed ROM (itself
    generated from the C++); used only for the collision non-vacuity
    check, never for expected values."""
    words = [
        int(line, 16)
        for line in CP_ROM_HEX.read_text().splitlines()
        if line and not line.startswith("//")
    ]
    base = ROM_BASE[fft_size]
    out = set()
    for w in words[base:]:
        if w == 0xFFFF:
            break
        out.add(w)
    return out


def run_golden(fft_size, pattern, num_active, symbols):
    """symbols: list of (symbol_index, samples). Returns a list of record
    lists, one per symbol."""
    if not PILOT_EXTRACTOR_GEN.exists():
        raise FileNotFoundError(
            f"{PILOT_EXTRACTOR_GEN} not found -- build it first: "
            "cmake --build build-fxp --target pilot_extractor_gen "
            "(requires -DATSC3_FIXED_POINT=ON -DATSC3_ENABLE_HDL_STUBS=ON)"
        )
    lines = [f"C {fft_size} {pattern} {num_active}"]
    for sym_idx, samples in symbols:
        assert len(samples) == fft_size
        lines.append(f"S {sym_idx}")
        lines += [f"{re} {im}" for re, im in samples]
    proc = subprocess.run(
        [str(PILOT_EXTRACTOR_GEN)],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        check=True,
    )
    out = []
    for line in proc.stdout.splitlines():
        if line.startswith("S "):
            out.append([])
        else:
            out[-1].append(tuple(int(v) for v in line.split()))
    assert len(out) == len(symbols)
    return out


async def reset_dut(dut, fft_size, pattern, num_active, flags=(0, 0, 0)):
    dut.rst.value = 1
    dut.cfg_fft_size.value = fft_size
    dut.cfg_pilot_pattern.value = pattern
    dut.cfg_num_active_carriers.value = num_active
    dut.s_axis_tvalid.value = 0
    dut.s_axis_tdata.value = 0
    dut.s_axis_tlast.value = 0
    dut.s_axis_tuser.value = 0
    dut.m_axis_tready.value = 1
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst.value = 0
    await RisingEdge(dut.clk)
    got = (
        int(dut.cfg_fft_size_invalid.value),
        int(dut.cfg_pattern_invalid.value),
        int(dut.cfg_num_active_invalid.value),
    )
    assert got == flags, f"config flags {got}, expected {flags}"
    assert dut.framing_error.value == 0


async def drive_input(dut, symbols, rnd, gaps, bad_tlast_at=None):
    """Bin 0 carries the symbol index on TUSER; every other bin carries a
    random value the DUT must ignore. bad_tlast_at = (symbol, bin) moves
    that symbol's TLAST to the given bin."""
    for s, (sym_idx, samples) in enumerate(symbols):
        n = len(samples)
        for k, (re, im) in enumerate(samples):
            if gaps and rnd.random() < 0.05:
                dut.s_axis_tvalid.value = 0
                for _ in range(rnd.randint(1, 3)):
                    await RisingEdge(dut.clk)
            last = k == n - 1
            if bad_tlast_at is not None and bad_tlast_at[0] == s:
                last = k == bad_tlast_at[1]
            dut.s_axis_tvalid.value = 1
            dut.s_axis_tdata.value = pack(re, im)
            dut.s_axis_tlast.value = int(last)
            dut.s_axis_tuser.value = (sym_idx & 15) if k == 0 else rnd.randint(0, 15)
            while True:
                await RisingEdge(dut.clk)
                if dut.s_axis_tready.value == 1:
                    break
    dut.s_axis_tvalid.value = 0


async def drive_backpressure(dut, rnd):
    while True:
        dut.m_axis_tready.value = int(rnd.random() < 0.6)
        await RisingEdge(dut.clk)


async def collect_output(dut, expected):
    """expected: list of (record, last). Checks the hold rule too."""
    idx = 0
    stalled = None
    while idx < len(expected):
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value != 1:
            assert stalled is None, f"record {idx}: TVALID dropped while stalled"
            continue
        word = (int(dut.m_axis_tdata.value), int(dut.m_axis_tlast.value))
        if stalled is not None:
            assert word == stalled, f"record {idx}: data changed while stalled"
        if dut.m_axis_tready.value != 1:
            stalled = word
            continue
        stalled = None
        exp_rec, exp_last = expected[idx]
        got = unpack_pilot(word[0])
        assert got == exp_rec, f"record {idx}: expected {exp_rec}, got {got}"
        assert word[1] == int(exp_last), f"record {idx}: TLAST expected {int(exp_last)}"
        idx += 1


def random_symbols(rnd, fft_size, indices):
    return [
        (
            s,
            [
                (rnd.randint(-32768, 32767), rnd.randint(-32768, 32767))
                for _ in range(fft_size)
            ],
        )
        for s in indices
    ]


async def run_stream(
    dut,
    fft_size,
    pattern,
    num_active,
    symbols,
    rnd,
    stalls=False,
    cfg=None,
    flags=(0, 0, 0),
):
    """Feeds symbols through one reset and checks every record. cfg
    overrides what is programmed (fft_size, pattern, num_active) when it
    differs from the golden configuration. Returns the golden records."""
    golden = run_golden(fft_size, pattern, num_active, symbols)
    expected = []
    for recs in golden:
        assert recs, "golden produced no pilots"
        expected += [(r, i == len(recs) - 1) for i, r in enumerate(recs)]

    await reset_dut(dut, *(cfg or (fft_size, pattern, num_active)), flags=flags)
    bp = cocotb.start_soon(drive_backpressure(dut, rnd)) if stalls else None
    drv = cocotb.start_soon(drive_input(dut, symbols, rnd, stalls))
    await collect_output(dut, expected)
    await drv
    if bp is not None:
        bp.kill()
        dut.m_axis_tready.value = 1
    for _ in range(3):
        await RisingEdge(dut.clk)
    assert dut.m_axis_tvalid.value == 0, "extra record after the last expected one"
    assert dut.framing_error.value == 0
    return golden


def collisions(fft_size, num_active, golden):
    """(scattered-on-continual, scattered-on-edge) record counts."""
    first = (fft_size - num_active) // 2
    last = first + num_active - 1
    cps = continual_offsets(fft_size)
    sp_cp = sp_edge = 0
    for recs in golden:
        for carrier, ptype, *_ in recs:
            if ptype == SCATTERED and (carrier - first) in cps:
                sp_cp += 1
            if ptype == SCATTERED and carrier in (first, last):
                sp_edge += 1
    return sp_cp, sp_edge


@cocotb.test(timeout_time=2000, timeout_unit="ms")
async def all_patterns_8k(dut):
    """PP1-PP8 at 8K: the largest scattered offset (symbol Dy-1) and the
    wrap to offset 0 (symbol Dy), plus PP1 symbol 1, the only 8K case
    where a scattered pilot lands on a continual carrier. Both collision
    kinds must occur."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x9105)
    sp_cp = sp_edge = 0
    for pattern in range(1, 9):
        dy = DY[pattern]
        indices = [dy - 1, dy] + ([1] if pattern == 1 else [])
        symbols = random_symbols(rnd, 8192, indices)
        golden = await run_stream(dut, 8192, pattern, 0, symbols, rnd)
        a, b = collisions(8192, DEFAULT_ACTIVE[8192], golden)
        sp_cp += a
        sp_edge += b
    assert (
        sp_cp > 0 and sp_edge > 0
    ), f"collisions seen: {sp_cp} SP/CP, {sp_edge} SP/edge"


@cocotb.test(timeout_time=1000, timeout_unit="ms")
async def pp8_mid_offset_with_stalls(dut):
    """PP8 (Dy=16) on symbols 7 and 9 back to back, with input gaps and
    output backpressure."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x9108)
    await run_stream(
        dut, 8192, 8, 0, random_symbols(rnd, 8192, [7, 9]), rnd, stalls=True
    )


@cocotb.test(timeout_time=1000, timeout_unit="ms")
async def size_16k(dut):
    """16K, PP7, symbol 3: the 16K ROM region and Dx=24 at the largest
    offset."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x9116)
    golden = await run_stream(dut, 16384, 7, 0, random_symbols(rnd, 16384, [3]), rnd)
    assert collisions(16384, DEFAULT_ACTIVE[16384], golden)[0] > 0


@cocotb.test(timeout_time=1000, timeout_unit="ms")
async def size_32k(dut):
    """32K, PP1, symbol 2: the 32K ROM region (365 entries) and bin
    indices up to 32767."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x9132)
    golden = await run_stream(dut, 32768, 1, 0, random_symbols(rnd, 32768, [2]), rnd)
    assert collisions(32768, DEFAULT_ACTIVE[32768], golden)[0] > 0


@cocotb.test(timeout_time=1000, timeout_unit="ms")
async def active_window_edges(dut):
    """Every carrier active (first_active = 0, so bin 0 is classified the
    cycle its TUSER arrives), a single active carrier (first = last) and
    an odd custom width."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x91AE)
    await run_stream(dut, 8192, 2, 8192, random_symbols(rnd, 8192, [0, 1]), rnd)
    await run_stream(dut, 8192, 3, 1, random_symbols(rnd, 8192, [0]), rnd)
    await run_stream(dut, 8192, 5, 7001, random_symbols(rnd, 8192, [2]), rnd)


@cocotb.test(timeout_time=1000, timeout_unit="ms")
async def invalid_config_falls_back(dut):
    """An undefined FFT size runs as 8K, pattern 0 and 9 run as PP3, and an
    active width beyond the FFT size runs as the default, each with its
    flag set."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x91CF)
    syms = random_symbols(rnd, 8192, [1])
    await run_stream(dut, 8192, 4, 0, syms, rnd, cfg=(12345, 4, 0), flags=(1, 0, 0))
    await run_stream(dut, 8192, 3, 0, syms, rnd, cfg=(8192, 0, 0), flags=(0, 1, 0))
    await run_stream(dut, 8192, 3, 0, syms, rnd, cfg=(8192, 9, 0), flags=(0, 1, 0))
    await run_stream(dut, 8192, 6, 0, syms, rnd, cfg=(8192, 6, 9000), flags=(0, 0, 1))


@cocotb.test(timeout_time=500, timeout_unit="ms")
async def misplaced_tlast_flags_framing_error(dut):
    """A TLAST on the wrong bin sets the sticky framing_error; the bin
    count, and so the records, are unaffected."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x91F7)
    symbols = random_symbols(rnd, 8192, [0])
    golden = run_golden(8192, 3, 0, symbols)
    expected = [(r, i == len(golden[0]) - 1) for i, r in enumerate(golden[0])]
    await reset_dut(dut, 8192, 3, 0)
    drv = cocotb.start_soon(
        drive_input(dut, symbols, rnd, False, bad_tlast_at=(0, 4000))
    )
    await collect_output(dut, expected)
    await drv
    assert dut.framing_error.value == 1, "misplaced TLAST not flagged"


@cocotb.test(timeout_time=500, timeout_unit="ms")
async def reset_mid_symbol(dut):
    """Reset partway through a symbol (with a record stalled on the
    output), then a clean symbol must come out exactly."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x91E5)
    await reset_dut(dut, 8192, 1, 0)
    dut.m_axis_tready.value = 0
    for k in range(3000):
        dut.s_axis_tvalid.value = 1
        dut.s_axis_tdata.value = pack(k, -k)
        dut.s_axis_tuser.value = 3
        await RisingEdge(dut.clk)
        if dut.m_axis_tvalid.value == 1 and k > 1000:
            break
    assert dut.m_axis_tvalid.value == 1, "no record stalled before the reset"
    dut.s_axis_tvalid.value = 0
    await run_stream(dut, 8192, 1, 0, random_symbols(rnd, 8192, [3]), rnd)
