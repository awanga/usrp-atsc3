"""cocotb testbench for hdl/rtl/ofdm/pilot_extractor.v

Bit-exact comparison against the real golden model
(lib/ofdm/pilot_extractor.cc's PilotExtractor::extract(),
ATSC3_FIXED_POINT=ON) via the hdl/sim/golden/pilot_extractor_gen CLI:
every PilotSymbol field (carrier, type, reference, received value) and
TLAST on each symbol's last pilot.

Covers every scattered pilot pattern that Annex D allows at 8K, each on
the symbol with the largest scattered offset (symbol Dy-1) and the symbol
after it (offset 0, where the first scattered pilot lands on the edge
carrier); 16K and 32K; every carrier reduction at 8K, so the common
continual pilots shift and drop out, with the odd-reduction rules for
the parenthesized additional pilots (8K SP32_4 / SP32_2, 16K SP32_4);
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

# Pattern codes are L1D_scattered_pilot_pattern values: 0 = SP3_2, 1 = SP3_4,
# 2 = SP4_2, ... 14 = SP32_2, 15 = SP32_4; even codes have DY = 2, odd DY = 4.
# SP24 (codes 12, 13) has no 8K layout; 32K allows only DY = 2 patterns other
# than SP4_2 (codes 0, 4, 6, 8, 10, 12, 14).
DY = {code: 2 + 2 * (code % 2) for code in range(16)}
NOC_MAX = {8192: 6913, 16384: 13825, 32768: 27649}
CUNIT = {8192: 96, 16384: 192, 32768: 384}
DEFAULT_ACTIVE = NOC_MAX
ALLOWED_8K = [c for c in range(16) if c not in (12, 13)]
ALLOWED_32K = [0, 4, 6, 8, 10, 12, 14]
SP6_2 = 4  # the fallback pattern, allowed at every FFT size
SCATTERED, CONTINUAL, EDGE = 0, 1, 2


# Table D.1.4, 8K and 32K columns at Cred_coeff 0: the additional continual
# pilots, which sit on scattered-pilot-bearing carriers (the common CPs
# never do). Used for the collision non-vacuity check only.
ADDITIONAL = {
    8192: {
        0: [1731], 1: [1731, 2886, 5733], 2: [1732], 3: [1732, 2888, 5724],
        4: [1734], 5: [1734, 2892, 5730], 6: [1736], 7: [1736, 2896, 5720],
        8: [1740], 9: [1740, 2904, 5748], 10: [1744], 11: [1744, 2912, 5744],
        14: [1696], 15: [1696, 2880, 5728],
    },
    32768: {0: [6939], 14: [6944]},
}


def active(fft_size, reduction):
    return NOC_MAX[fft_size] - reduction * CUNIT[fft_size]


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


def continual_offsets(fft_size, reduction=0):
    """Relative common continual-pilot carriers from the committed CP32 ROM
    (itself generated from the C++) and the standard's derivation of the
    16K / 8K sets; used only for the collision non-vacuity check, never for
    expected values."""
    words = [
        int(line, 16)
        for line in CP_ROM_HEX.read_text().splitlines()
        if line and not line.startswith("//")
    ]
    stride = 32768 // fft_size
    shift = reduction * CUNIT[fft_size] // 2
    out = set()
    for w in words[::stride]:
        if w == 0xFFFF:
            break
        absolute = (w + stride - 1) // stride
        if absolute >= shift:
            out.add(absolute - shift)
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


def collisions(fft_size, num_active, golden, pattern=None):
    """(scattered-on-continual, scattered-on-edge) record counts, the
    continual carriers being the common set and the pattern's additional
    pilots."""
    first = (fft_size - num_active) // 2
    last = first + num_active - 1
    reduction = (NOC_MAX[fft_size] - num_active) // CUNIT[fft_size]
    cps = continual_offsets(fft_size, reduction)
    cps |= set(ADDITIONAL.get(fft_size, {}).get(pattern, []))
    sp_cp = sp_edge = 0
    for recs in golden:
        for carrier, ptype, *_ in recs:
            if ptype == SCATTERED and (carrier - first) in cps:
                sp_cp += 1
            if ptype == SCATTERED and carrier in (first, last):
                sp_edge += 1
    return sp_cp, sp_edge


@cocotb.test(timeout_time=3000, timeout_unit="ms")
async def all_patterns_8k(dut):
    """Every pattern allowed at 8K, over a full scattered-pilot period and
    the wrap to offset 0 (symbol Dy). Both collision kinds (scattered on an
    additional continual carrier, scattered on an edge carrier) must occur
    across the sweep."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x9105)
    sp_cp = sp_edge = 0
    for pattern in ALLOWED_8K:
        dy = DY[pattern]
        symbols = random_symbols(rnd, 8192, list(range(dy)) + [dy])
        golden = await run_stream(dut, 8192, pattern, 0, symbols, rnd)
        a, b = collisions(8192, DEFAULT_ACTIVE[8192], golden, pattern)
        sp_cp += a
        sp_edge += b
    assert (
        sp_cp > 0 and sp_edge > 0
    ), f"collisions seen: {sp_cp} SP/CP, {sp_edge} SP/edge"


@cocotb.test(timeout_time=1000, timeout_unit="ms")
async def sp12_4_mid_offset_with_stalls(dut):
    """SP12_4 (Dy=4) on symbols 1 and 2 back to back, with input gaps and
    output backpressure."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x9108)
    await run_stream(
        dut, 8192, 9, 0, random_symbols(rnd, 8192, [1, 2]), rnd, stalls=True
    )


@cocotb.test(timeout_time=2000, timeout_unit="ms")
async def size_16k(dut):
    """16K: the stride-2 CP32 walk, with a Dy=4 and a Dy=2 pattern and the
    pattern SP24_4 that only 16K and larger allow."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x9116)
    await run_stream(dut, 16384, 13, 0, random_symbols(rnd, 16384, [3]), rnd)
    await run_stream(dut, 16384, 14, 0, random_symbols(rnd, 16384, [1]), rnd)


@cocotb.test(timeout_time=3000, timeout_unit="ms")
async def size_32k(dut):
    """32K (stride 1, bin indices up to 32767): SP3_2 (largest pilot
    density) and SP32_2."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x9132)
    golden = await run_stream(dut, 32768, 0, 0, random_symbols(rnd, 32768, [1]), rnd)
    await run_stream(dut, 32768, 14, 0, random_symbols(rnd, 32768, [0]), rnd)
    assert collisions(32768, DEFAULT_ACTIVE[32768], golden, 0)[0] > 0


@cocotb.test(timeout_time=4000, timeout_unit="ms")
async def carrier_reduction(dut):
    """Every carrier reduction at 8K: the active window narrows, so the
    first CP32 entries drop out and the rest shift (the pointer skips them
    while the pre-window bins go by). 8K SP32_4 (Table D.1.5) and SP32_2
    exercise the parenthesized additional pilots at odd reductions, and
    16K SP32_4 / 32K at the deepest reductions the other sizes."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x91AE)
    for reduction in range(1, 5):
        pattern = 15 if reduction % 2 else 14
        n = active(8192, reduction)
        await run_stream(dut, 8192, pattern, n, random_symbols(rnd, 8192, [reduction % 4]), rnd)
    await run_stream(
        dut, 8192, 15, active(8192, 4), random_symbols(rnd, 8192, [2]), rnd, stalls=True
    )
    await run_stream(dut, 16384, 15, active(16384, 3), random_symbols(rnd, 16384, [1]), rnd)
    await run_stream(dut, 32768, 4, active(32768, 4), random_symbols(rnd, 32768, [1]), rnd)


@cocotb.test(timeout_time=2000, timeout_unit="ms")
async def invalid_config_falls_back(dut):
    """An undefined FFT size runs as 8K; a pattern the FFT size does not
    allow (SP24_2 at 8K, SP3_4 at 32K) runs as SP6_2; a carrier count that
    is not NoCmax - Cred_coeff * Cunit runs as the default; each with its
    flag set."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x91CF)
    syms = random_symbols(rnd, 8192, [1])
    await run_stream(dut, 8192, 4, 0, syms, rnd, cfg=(12345, 4, 0), flags=(1, 0, 0))
    await run_stream(dut, 8192, SP6_2, 0, syms, rnd, cfg=(8192, 12, 0), flags=(0, 1, 0))
    await run_stream(dut, 8192, SP6_2, 0, syms, rnd, cfg=(8192, 13, 0), flags=(0, 1, 0))
    await run_stream(dut, 8192, 6, 0, syms, rnd, cfg=(8192, 6, 9000), flags=(0, 0, 1))
    await run_stream(dut, 8192, 6, 0, syms, rnd, cfg=(8192, 6, 6913 - 50), flags=(0, 0, 1))
    await run_stream(dut, 8192, 6, 0, syms, rnd, cfg=(8192, 6, 6913 - 5 * 96), flags=(0, 0, 1))
    await run_stream(dut, 8192, 6, 0, syms, rnd, cfg=(8192, 6, 6914), flags=(0, 0, 1))
    syms32 = random_symbols(rnd, 32768, [0])
    await run_stream(dut, 32768, SP6_2, 0, syms32, rnd, cfg=(32768, 1, 0), flags=(0, 1, 0))


@cocotb.test(timeout_time=500, timeout_unit="ms")
async def misplaced_tlast_flags_framing_error(dut):
    """A TLAST on the wrong bin sets the sticky framing_error; the bin
    count, and so the records, are unaffected."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    rnd = random.Random(0x91F7)
    symbols = random_symbols(rnd, 8192, [0])
    golden = run_golden(8192, 5, 0, symbols)
    expected = [(r, i == len(golden[0]) - 1) for i, r in enumerate(golden[0])]
    await reset_dut(dut, 8192, 5, 0)
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
