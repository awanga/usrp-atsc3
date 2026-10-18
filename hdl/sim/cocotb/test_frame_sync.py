"""cocotb testbench for hdl/rtl/sync/frame_sync.v (built at ADDR_W=11: a
2048-sample chunk RAM).

Bit-exact comparison against the real golden model
(lib/sync/frame_sync.cc, ATSC3_FIXED_POINT=ON) via the
hdl/sim/golden/frame_sync_gen CLI. Pinned convention: a chunk is the
TLAST-delimited packet, and the model gets the same chunk in one
FrameSync::process() call. After every bootstrap offset, chunk and reset
the bench compares the whole synchronizer state (state, frame number,
expected boundary, confidence, samples to the boundary) and every
FrameEvent (type, sample index, frame number, confidence) with the model.

Scenarios are random scripts over small preambles (the model's reference
repeats every 13 samples, so 13..26-sample correlations exercise every
state quickly): bootstrap offsets placed just ahead of the sample counter,
chunks that are exact reference copies, partial copies, reference plus
noise, noise and silence, so the synchronizer searches, acquires, locks,
tracks, misses, holds over, reacquires and gives up. The configuration
varies per script (fft size, window, threshold including 0, lock counts,
holdover frames); because the model's ACQUIRING loop only succeeds when
fft_size fits the search window, some scripts never leave ACQUIRING and
that is checked too. Input gaps, event backpressure (hold rule checked)
and configuration scrambled while a chunk is processed run throughout.
Aggregate counters assert every transition kind occurred.

Directed: bootstrap and sample presented together, bootstrap refused
mid-chunk, unusable configuration and oversized chunks (flagged, only the
sample counter advances), reset in the middle of a chunk's processing.

Run via test_runner.py (pytest), not directly. HDL_FULL=1 adds a script at
the real 8K size.
"""

import os
import random

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import FallingEdge, RisingEdge

from test_frame_correlator_tb import gen, make_signal, model_reference, pack

FULL = os.environ.get("HDL_FULL") == "1"
DEPTH = 1 << 11

# P-line fields: fft cp symbols subframes threshold acquire loss holdover window
CFG_PORTS = (
    "cfg_fft_size",
    "cfg_cp_length",
    "cfg_symbols_per_subframe",
    "cfg_subframes_per_frame",
    "cfg_threshold_q15",
    "cfg_lock_acquire_count",
    "cfg_lock_loss_count",
    "cfg_holdover_frames",
    "cfg_search_window",
)

# FrameEvent layout (status_words.vh)
EV_FIELDS = {
    "type": (0, 8),
    "sample_index": (8, 48),
    "frame": (56, 32),
    "subframe": (88, 16),
    "symbol": (104, 16),
    "confidence": (120, 16),
}


def ev_field(word, name):
    lsb, width = EV_FIELDS[name]
    return (word >> lsb) & ((1 << width) - 1)


def run_golden(script):
    """script: ("P", cfg) | ("B", idx, conf) | ("K", samples) | ("R",).
    Returns, per op, (events, state) where events is a list of
    (type, sample_index, frame, sub, sym, conf) and state is the T tuple
    (state, frame, expected, conf, samples_to_boundary); None for P."""
    lines = []
    for op in script:
        if op[0] == "P":
            lines.append("P " + " ".join(str(v) for v in op[1]))
        elif op[0] == "B":
            lines.append(f"B {op[1]} {op[2]}")
        elif op[0] == "K":
            lines.append(f"K {len(op[1])}")
            lines += [f"{re_} {im}" for re_, im in op[1]]
        else:
            lines.append("R")
    out = gen(lines)
    results = []
    pos = 0
    for op in script:
        if op[0] == "P":
            results.append(([], None))
            continue
        events = []
        while out[pos].startswith("E"):
            events.append(tuple(int(v) for v in out[pos].split()[1:]))
            pos += 1
        assert out[pos].startswith("T"), out[pos]
        state = tuple(int(v) for v in out[pos].split()[1:])
        pos += 1
        results.append((events, state))
    assert pos == len(out)
    return results


class Bench:
    def __init__(self, dut, rnd, stalls=True, gaps=True, scramble=True):
        self.dut = dut
        self.rnd = rnd
        self.stalls = stalls
        self.gaps = gaps
        self.scramble = scramble
        self.events = []
        self.cfg = None

    async def start(self):
        cocotb.start_soon(Clock(self.dut.clk, 10, units="ns").start())
        self.dut.s_axis_tvalid.value = 0
        self.dut.s_axis_tdata.value = 0
        self.dut.s_axis_tlast.value = 0
        self.dut.bs_valid.value = 0
        self.dut.bs_offset.value = 0
        self.dut.bs_conf_q15.value = 0
        self.dut.m_axis_tready.value = 1
        self.set_cfg((26, 4, 3, 2, 22937, 3, 5, 10, 64))
        cocotb.start_soon(self.collector())

    def set_cfg(self, cfg):
        self.cfg = tuple(cfg)
        for name, v in zip(CFG_PORTS, cfg):
            getattr(self.dut, name).value = v & 0xFFFF

    async def reset(self, cfg=None):
        """FrameSync construction (P) or reset(): rst high for a few cycles."""
        if cfg is not None:
            self.set_cfg(cfg)
        await FallingEdge(self.dut.clk)
        self.dut.rst.value = 1
        for _ in range(3):
            await RisingEdge(self.dut.clk)
        await FallingEdge(self.dut.clk)
        self.dut.rst.value = 0
        await RisingEdge(self.dut.clk)
        self.events.clear()

    async def collector(self):
        """Event sink with random backpressure; checks the hold rule. The
        ready chosen at a falling edge is what the next rising edge sees, so
        the transfer is decided here, with the valid/data read at the same
        falling edge."""
        dut = self.dut
        stalled = None
        while True:
            await FallingEdge(dut.clk)
            valid = dut.m_axis_tvalid.value == 1
            word = int(dut.m_axis_tdata.value) if valid else None
            if stalled is not None and dut.rst.value == 0:
                assert valid, "m_axis_tvalid dropped while stalled"
                assert word == stalled, "event changed while stalled"
            ready = (self.rnd.random() < 0.4) if self.stalls else True
            dut.m_axis_tready.value = int(ready)
            if valid and ready:
                assert dut.m_axis_tlast.value == 1, "event TLAST"
                self.events.append(word)
                stalled = None
            else:
                stalled = word

    async def wait_idle(self, limit=2_000_000):
        """Until the synchronizer accepts samples again (event drained)."""
        dut = self.dut
        for _ in range(limit):
            await FallingEdge(dut.clk)
            if dut.s_axis_tready.value == 1:
                return
        raise AssertionError(f"synchronizer still busy after {limit} cycles")

    async def send_bootstrap(self, idx, conf):
        dut = self.dut
        await self.wait_idle()
        await RisingEdge(dut.clk)  # drive just after an edge, sample at the falling one
        dut.bs_valid.value = 1
        dut.bs_offset.value = idx
        dut.bs_conf_q15.value = conf
        while True:
            await FallingEdge(dut.clk)
            accepted = dut.bs_ready.value == 1
            await RisingEdge(dut.clk)
            if accepted:
                break
        dut.bs_valid.value = 0

    async def send_chunk(self, samples):
        dut = self.dut
        await self.wait_idle()
        await RisingEdge(dut.clk)  # drive just after an edge, sample at the falling one
        for i, (re_, im) in enumerate(samples):
            if self.gaps and self.rnd.random() < 0.15:
                dut.s_axis_tvalid.value = 0
                for _ in range(self.rnd.randint(1, 4)):
                    await RisingEdge(dut.clk)
            dut.s_axis_tvalid.value = 1
            dut.s_axis_tdata.value = pack(re_, im)
            dut.s_axis_tlast.value = int(i == len(samples) - 1)
            while True:
                await FallingEdge(dut.clk)
                accepting = dut.s_axis_tready.value == 1
                await RisingEdge(dut.clk)
                if accepting:
                    break
        dut.s_axis_tvalid.value = 0
        dut.s_axis_tlast.value = 0
        if self.scramble:
            # The configuration was sampled on the TLAST edge: changing it
            # while the chunk is processed must have no effect.
            for name in CFG_PORTS:
                getattr(dut, name).value = self.rnd.getrandbits(16)
        await self.wait_idle()
        if self.scramble:
            self.set_cfg(self.cfg)
            await FallingEdge(dut.clk)

    @staticmethod
    def describe(ops):
        return " ".join(
            op[0]
            + (str(len(op[1])) if op[0] == "K" else str(op[1]) if op[0] == "B" else "")
            for op in ops
        )

    def monitors(self):
        dut = self.dut
        return (
            int(dut.mon_state.value),
            int(dut.mon_frame_number.value),
            int(dut.mon_expected_boundary.value),
            int(dut.mon_confidence.value),
            int(dut.mon_samples_to_boundary.value),
        )

    def check(self, what, want_events, want_state):
        got_events = [
            (
                ev_field(w, "type"),
                ev_field(w, "sample_index"),
                ev_field(w, "frame"),
                ev_field(w, "subframe"),
                ev_field(w, "symbol"),
                ev_field(w, "confidence"),
            )
            for w in self.events
        ]
        assert (
            got_events == want_events
        ), f"{what}: events {got_events}, model {want_events}"
        got = self.monitors()
        assert got == want_state, f"{what}: state {got}, model {want_state}"
        self.events.clear()

    async def play(self, script):
        results = run_golden(script)
        for n, (op, (want_events, want_state)) in enumerate(zip(script, results)):
            what = f"op {n} {op[0]} (cfg {self.cfg}, {self.describe(script[:n + 1])})"
            if op[0] == "P":
                await self.reset(op[1])
                continue
            if op[0] == "R":
                await self.reset()
            elif op[0] == "B":
                await self.send_bootstrap(op[1], op[2])
            else:
                await self.send_chunk(op[1])
            await self.wait_idle()
            self.check(what, want_events, want_state)
        return results


# ------------------------------------------------------------------------------
# Script generation
# ------------------------------------------------------------------------------

CFG_CHOICES = [
    # fft cp sym sub thr acquire loss holdover window
    (26, 4, 3, 2, 22937, 3, 5, 10, 40),
    (26, 4, 3, 2, 22937, 2, 2, 3, 32),
    (13, 3, 2, 3, 22937, 1, 1, 2, 16),
    (13, 3, 2, 3, 12000, 2, 3, 4, 26),
    (16, 2, 4, 1, 22937, 2, 2, 2, 40),
    (26, 4, 3, 2, 0, 2, 2, 2, 32),
    (26, 4, 3, 2, 32767, 1, 1, 2, 32),
    (39, 7, 1, 5, 18000, 2, 2, 3, 44),
    (64, 8, 1, 1, 22937, 2, 2, 2, 16),  # fft > window: ACQUIRING never succeeds
]


def random_script(rnd, ref, n_ops):
    cfg = rnd.choice(CFG_CHOICES)
    fft, window = cfg[0], cfg[8]
    script = [("P", cfg)]
    sc = 0  # samples the model has consumed
    good = True  # signal quality flips in runs, so the lock is built and lost
    for _ in range(n_ops):
        r = rnd.random()
        if r < 0.03:
            script.append(("R",))
            sc = 0
            continue
        if rnd.random() < 0.25:
            good = not good
        n = fft + rnd.choice([0, 1, 3, 8, 16, 33, window])
        if rnd.random() < 0.04:
            n = max(1, fft - rnd.randint(1, fft - 1))  # shorter than the preamble
        if r < 0.7:
            # point the expected boundary into the coming chunk
            idx = sc + rnd.randint(0, max(0, n - fft)) + rnd.randint(0, 6)
            if rnd.random() < 0.1:
                idx = sc + n + rnd.randint(0, 40)  # beyond the chunk
            script.append(("B", idx, rnd.choice([0, 3277, 16384, 27852, 32767, 40000])))
        if good:
            kinds, weights = ["ref", "ref_noise", "partial", "delayed"], [8, 3, 1, 5]
        else:
            kinds, weights = [
                "zero",
                "noise_small",
                "noise_full",
                "partial",
                "ref_noise",
            ], [4, 1, 2, 2, 1]
        kind = rnd.choices(kinds, weights=weights)[0]
        script.append(("K", make_signal(rnd, kind, n, ref)))
        sc += n
    return script


def tally(script, results, counts):
    """Counts state transitions seen in the model's trace."""
    state = 0
    for op, (events, st) in zip(script, results):
        if st is None:
            state = 0
            continue
        new = st[0]
        counts[(state, new)] = counts.get((state, new), 0) + 1
        if events:
            counts["events"] = counts.get("events", 0) + len(events)
            if state == 3 and new == 2:
                counts["holdover_relock"] = counts.get("holdover_relock", 0) + 1
        state = new


@cocotb.test(timeout_time=900, timeout_unit="sec")
async def random_scripts_match_model(dut):
    """Random scripts, compared after every operation."""
    rnd = random.Random(0xF5A1)
    ref = model_reference()
    bench = Bench(dut, rnd)
    await bench.start()
    counts = {}
    n_scripts = 40 if FULL else 12
    for s in range(n_scripts):
        script = random_script(rnd, ref, 14 if not FULL else 30)
        results = await bench.play(script)
        tally(script, results, counts)
    dut._log.info("transitions: %s", counts)
    must = {
        (0, 1): "SEARCHING -> ACQUIRING",
        (1, 2): "ACQUIRING -> LOCKED",
        (1, 0): "ACQUIRING -> SEARCHING",
        (2, 3): "LOCKED -> HOLDOVER",
    }
    for key, name in must.items():
        assert counts.get(key, 0) > 0, f"stimulus never produced {name}"
    assert counts.get("events", 0) >= 5, "too few FrameEvents compared"
    assert counts.get((2, 2), 0) > 0, "no LOCKED chunk compared"


@cocotb.test(timeout_time=600, timeout_unit="sec")
async def directed_lock_and_loss(dut):
    """The documented walk, event by event: search, acquire, lock, track,
    miss, holdover decay, reacquire, give up."""
    rnd = random.Random(1)
    ref = model_reference()
    bench = Bench(dut, rnd, stalls=False, gaps=False, scramble=False)
    await bench.start()
    cfg = (26, 4, 3, 2, 22937, 2, 1, 2, 64)
    good = make_signal(rnd, "ref", 66, ref)
    silence = [(0, 0)] * 66
    script = [
        ("P", cfg),
        ("K", good),  # SEARCHING -> ACQUIRING
        ("B", 66 + 20, 16384),
        ("K", good),  # second hit: LOCKED + event
        ("B", 132 + 20, 16384),
        ("K", good),  # tracked: event, frame 1
        ("B", 198 + 20, 16384),
        ("K", silence),  # relaxed threshold missed, loss count 1: HOLDOVER
        ("K", silence),  # holdover 1: decay
        ("K", silence),  # holdover 2: decay
        ("K", silence),  # counter >= 2: back to SEARCHING
    ]
    results = await bench.play(script)
    # Expected walk, from the state machine's definition (thresholds 0.7 /
    # 0.56, acquire 2, loss 1, holdover 2), not from the model's output:
    states = [r[1][0] for r in results[1:]]
    assert states == [1, 1, 2, 2, 2, 2, 3, 3, 0, 0], states
    ev1, ev2 = [e for r in results for e in r[0]]
    assert ev1 == (0, 66, 0, 0, 0, 32767), ev1  # FRAME_START at the lock, +1.0
    assert ev2[:5] == (0, 132, 0, 0, 0), ev2  # frame number counts after the event
    assert 17000 < ev2[5] < 17300, ev2  # (19 * 16384 + ~32768) / 20
    confs = [r[1][3] for r in results[1:]]
    assert confs[6] == 16384, "LOCKED miss must keep the bootstrap confidence"
    assert confs[7] == 14745, "first holdover decay: 9 * 16384 / 10"
    assert confs[8] == 0 and confs[9] == 0, "SEARCHING clears the confidence"


@cocotb.test(timeout_time=600, timeout_unit="sec")
async def directed_search_step(dut):
    """SEARCHING tries offsets 0, F/4, F/2, ...: with a 26-sample preamble
    that is every 6 samples. A reference that starts 28 samples into the
    chunk first correlates at 0.7 once 13 of the window's 26 samples match,
    i.e. from offset 15; the first multiple of 6 from there is 18."""
    rnd = random.Random(11)
    ref = model_reference()
    bench = Bench(dut, rnd, stalls=False, gaps=False)
    await bench.start()
    cfg = (26, 4, 3, 2, 22937, 2, 5, 10, 64)
    chunk = [(0, 0)] * 28 + [ref[i % 13] for i in range(52)]
    results = await bench.play([("P", cfg), ("K", chunk)])
    state, _, expected, _, _ = results[1][1]
    assert state == 1 and expected == 18, (state, expected)


@cocotb.test(timeout_time=600, timeout_unit="sec")
async def directed_holdover_relock(dut):
    """HOLDOVER predicts the next boundary one frame (180 samples) after the
    last one, and correlates there when the prediction falls inside a chunk
    with room for the preamble. With 100-sample chunks and the last
    boundary 30 samples into the missed chunk (sample 230), the prediction
    410 is 10 samples into the second holdover chunk: a good preamble there
    relocks without a bootstrap offset, with the frame number advanced by
    the one predicted frame."""
    rnd = random.Random(6)
    ref = model_reference()
    bench = Bench(dut, rnd, stalls=False, gaps=False)
    await bench.start()
    cfg = (26, 4, 3, 2, 22937, 1, 1, 6, 64)
    good = make_signal(rnd, "ref", 100, ref)
    silence = [(0, 0)] * 100
    script = [("P", cfg), ("K", good), ("B", 100 + 20, 16384), ("K", good)]
    script += [("B", 200 + 30, 16384), ("K", silence)]  # LOCKED miss -> HOLDOVER
    script += [("K", silence)]  # prediction 230 is behind: 230 + 180 = 410
    script += [("K", good)]  # chunk [400, 500): relock
    results = await bench.play(script)
    states = [r[1][0] for r in results[1:]]
    assert states == [1, 1, 2, 2, 3, 3, 2], states
    events = [e for r in results for e in r[0]]
    assert len(events) == 2, events
    assert events[1][:5] == (0, 400, 1, 0, 0), events[1]
    assert events[1][5] == 32767


def partial_chunk(ref, n, m):
    """First m samples copy the reference, the rest are zero: correlates at
    sqrt(m / fft_size) at offset 0 and lower elsewhere."""
    return [ref[i % 13] if i < m else (0, 0) for i in range(n)]


@cocotb.test(timeout_time=600, timeout_unit="sec")
async def directed_thresholds(dut):
    """The strict threshold (0.7) applies while searching and acquiring, 4/5
    of it while locked: with a 26-sample preamble, 8 matching samples give
    0.555 (below 0.56: a locked miss), 12 give 0.679 (a locked hit, but not
    a detection) and 14 give 0.734 (a detection)."""
    rnd = random.Random(8)
    ref = model_reference()
    bench = Bench(dut, rnd, stalls=False, gaps=False)
    await bench.start()
    cfg = (26, 4, 3, 2, 22937, 2, 5, 10, 64)
    full = make_signal(rnd, "ref", 66, ref)
    script = [("P", cfg)]
    script += [("K", partial_chunk(ref, 66, 12))]  # 0.679 < 0.7: no detection
    script += [("K", partial_chunk(ref, 66, 14))]  # 0.734: ACQUIRING
    script += [("B", 66 * 2 + 20, 16384), ("K", full)]  # second hit: LOCKED
    script += [("B", 66 * 3 + 20, 16384), ("K", partial_chunk(ref, 66, 8))]  # miss
    script += [("B", 66 * 4 + 20, 16384), ("K", partial_chunk(ref, 66, 12))]  # hit
    results = await bench.play(script)
    states = [r[1][0] for r in results[1:]]
    assert states == [0, 1, 1, 2, 2, 2, 2, 2], states
    events = [e for r in results for e in r[0]]
    assert len(events) == 2, "locked miss must not emit, locked hit must"
    assert events[1][1] == 66 * 4


@cocotb.test(timeout_time=600, timeout_unit="sec")
async def directed_full_ram_chunk(dut):
    """A chunk of exactly the RAM depth is stored and processed; one sample
    more is refused (unusable_chunks_only_advance_the_counter)."""
    rnd = random.Random(9)
    ref = model_reference()
    bench = Bench(dut, rnd, stalls=False, gaps=False)
    await bench.start()
    cfg = (1000, 100, 1, 1, 22937, 2, 5, 10, 64)
    sig = make_signal(rnd, "ref", DEPTH, ref)
    results = await bench.play([("P", cfg), ("K", sig)])
    assert results[1][1][0] == 1, "a full-RAM chunk of reference was not processed"
    assert dut.chunk_overflow.value == 0 and dut.cfg_fft_size_invalid.value == 0


@cocotb.test(timeout_time=600, timeout_unit="sec")
async def directed_holdover_episodes(dut):
    """Two holdover episodes with a relock between: the lock-loss and
    holdover counters start from zero each time (loss count 2, holdover
    frames 3). A relock must clear the miss streak that caused the first
    episode, and the second episode must get its full three frames."""
    rnd = random.Random(10)
    ref = model_reference()
    bench = Bench(dut, rnd, stalls=False, gaps=False)
    await bench.start()
    cfg = (26, 4, 3, 2, 22937, 1, 2, 3, 64)
    good = make_signal(rnd, "ref", 100, ref)
    silence = [(0, 0)] * 100
    script = [("P", cfg), ("K", good), ("B", 120, 16384), ("K", good)]  # LOCKED
    script += [("B", 230, 16384), ("K", silence)]  # miss 1 of 2
    script += [("B", 330, 16384), ("K", silence)]  # miss 2: HOLDOVER
    script += [("K", silence)]  # holdover 1: boundary 330 -> 510
    script += [("K", good)]  # 510 is 10 into [500, 600): relock
    script += [("B", 630, 16384), ("K", silence)]  # one miss, not two
    script += [("B", 730, 16384), ("K", silence)]  # second miss: HOLDOVER again
    script += [("K", silence)] * 3  # holdover 1, 2, then 3 frames: SEARCHING
    results = await bench.play(script)
    states = [r[1][0] for r in results[1:]]
    assert states == [1, 1, 2, 2, 2, 2, 3, 3, 2, 2, 2, 2, 3, 3, 3, 0], states
    assert sum(len(r[0]) for r in results) == 2


@cocotb.test(timeout_time=600, timeout_unit="sec")
async def bootstrap_and_sample_together(dut):
    """A bootstrap offset and the next chunk's first sample presented
    together: the offset goes first (the C++ call order), the sample waits."""
    rnd = random.Random(2)
    ref = model_reference()
    bench = Bench(dut, rnd, stalls=False, gaps=False, scramble=False)
    await bench.start()
    cfg = (26, 4, 3, 2, 22937, 1, 5, 10, 64)
    good = make_signal(rnd, "ref", 66, ref)
    script = [("P", cfg), ("B", 20, 20000), ("K", good)]
    results = run_golden(script)
    await bench.reset(cfg)
    await bench.wait_idle()
    assert dut.bs_ready.value == 1

    dut.bs_valid.value = 1
    dut.bs_offset.value = 20
    dut.bs_conf_q15.value = 20000
    dut.s_axis_tvalid.value = 1
    dut.s_axis_tdata.value = pack(*good[0])
    dut.s_axis_tlast.value = 0
    await FallingEdge(dut.clk)
    assert dut.s_axis_tready.value == 0, "sample accepted in the bootstrap's cycle"
    await RisingEdge(dut.clk)  # the bootstrap offset is taken on this edge
    dut.bs_valid.value = 0
    # finish the chunk (its first sample is still presented)
    for i, (re_, im) in enumerate(good):
        if i > 0:
            dut.s_axis_tdata.value = pack(re_, im)
        dut.s_axis_tlast.value = int(i == len(good) - 1)
        while True:
            await FallingEdge(dut.clk)
            accepting = dut.s_axis_tready.value == 1
            if i == 0:
                assert bench.monitors()[2] == 20, "bootstrap offset not applied first"
            if accepting and i == 1:
                assert dut.bs_ready.value == 0, "bootstrap accepted mid-chunk"
            await RisingEdge(dut.clk)
            if accepting:
                break
    dut.s_axis_tvalid.value = 0
    await bench.wait_idle()
    bench.check("bootstrap+sample", results[2][0], results[2][1])


@cocotb.test(timeout_time=600, timeout_unit="sec")
async def unusable_chunks_only_advance_the_counter(dut):
    """fft_size below 4 or an oversized chunk is flagged and skipped; the
    sample counter still advances, so later events carry the right index."""
    rnd = random.Random(3)
    ref = model_reference()
    bench = Bench(dut, rnd, stalls=False, gaps=False, scramble=False)
    await bench.start()
    good_cfg = (26, 4, 3, 2, 22937, 1, 5, 10, 64)
    good = make_signal(rnd, "ref", 66, ref)

    # Model: lock on the second chunk, event at sample index 66.
    script = [("P", good_cfg), ("K", good), ("B", 66 + 20, 16384), ("K", good)]
    results = run_golden(script)
    want_events, want_state = results[3]
    assert len(want_events) == 1

    # RTL: a skipped chunk of 100 samples first, so everything shifts by 100.
    await bench.reset((2, 4, 3, 2, 22937, 1, 5, 10, 64))  # fft_size 2: unusable
    await bench.send_chunk([(1, 1)] * 100)
    assert dut.cfg_fft_size_invalid.value == 1, "bad fft_size not flagged"
    assert bench.events == [], "event from an unusable chunk"
    assert bench.monitors()[0] == 0

    # Oversized chunk (more than the 2048-sample RAM) with a usable config.
    bench.set_cfg(good_cfg)
    await bench.send_chunk([(5, 5)] * (DEPTH + 7))
    assert dut.chunk_overflow.value == 1, "oversized chunk not flagged"
    assert bench.events == []
    skipped = 100 + DEPTH + 7

    await bench.send_chunk(good)
    await bench.send_bootstrap(skipped + 66 + 20, 16384)
    await bench.send_chunk(good)
    await bench.wait_idle()
    shifted = [
        (t, idx + skipped, fr, sub, sym, c) for t, idx, fr, sub, sym, c in want_events
    ]
    st, frame, exp, conf, to_b = want_state
    bench.check("after skipped chunks", shifted, (st, frame, exp + skipped, conf, to_b))

    # Flags are sticky until reset.
    assert dut.cfg_fft_size_invalid.value == 1 and dut.chunk_overflow.value == 1
    await bench.reset()
    assert dut.cfg_fft_size_invalid.value == 0 and dut.chunk_overflow.value == 0


@cocotb.test(timeout_time=600, timeout_unit="sec")
async def reset_during_processing(dut):
    """Reset while a chunk is being processed: everything restarts from the
    constructed state; the abandoned chunk produces no event."""
    rnd = random.Random(4)
    ref = model_reference()
    bench = Bench(dut, rnd, stalls=False, gaps=False, scramble=False)
    await bench.start()
    cfg = (26, 4, 3, 2, 22937, 1, 5, 10, 64)
    good = make_signal(rnd, "ref", 66, ref)

    await bench.reset(cfg)
    await bench.send_chunk(good)
    await bench.send_bootstrap(66 + 20, 16384)
    # Feed the second chunk, then reset while its window search runs.
    for i, (re_, im) in enumerate(good):
        dut.s_axis_tvalid.value = 1
        dut.s_axis_tdata.value = pack(re_, im)
        dut.s_axis_tlast.value = int(i == len(good) - 1)
        while True:
            await FallingEdge(dut.clk)
            accepting = dut.s_axis_tready.value == 1
            await RisingEdge(dut.clk)
            if accepting:
                break
    dut.s_axis_tvalid.value = 0
    for _ in range(300):
        await RisingEdge(dut.clk)
    await FallingEdge(dut.clk)
    assert dut.mon_busy.value == 1, "chunk finished before the reset: weak test"
    await bench.reset()
    assert dut.s_axis_tready.value == 1 and dut.mon_busy.value == 0
    assert bench.monitors() == (0, 0, 0, 0, 0)
    assert bench.events == []

    script = [("P", cfg), ("K", good), ("B", 66 + 20, 16384), ("K", good)]
    results = run_golden(script)
    await bench.send_chunk(good)
    await bench.send_bootstrap(66 + 20, 16384)
    await bench.send_chunk(good)
    await bench.wait_idle()
    bench.check("after reset", results[3][0], results[3][1])


@cocotb.test(timeout_time=1500, timeout_unit="sec", skip=not FULL)
async def real_size_search(dut):
    """Nightly: an 8K-sample preamble, ACQUIRING with the default window
    (the model's loop cannot succeed at this size) and a miss streak."""
    rnd = random.Random(5)
    ref = model_reference()
    bench = Bench(dut, rnd, stalls=False, gaps=False, scramble=False)
    await bench.start()
    cfg = (8192, 1024, 1, 1, 22937, 3, 2, 10, 64)
    n = 8192 + 40
    script = [
        ("P", cfg),
        ("K", make_signal(rnd, "ref", n, ref)),
        ("B", n + 20, 20000),
        ("K", make_signal(rnd, "ref", n, ref)),
        ("K", make_signal(rnd, "noise_full", n, ref)),
    ]
    await bench.play(script)
