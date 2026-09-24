"""cocotb testbench for hdl/rtl/sync/bootstrap_detector.v

Bit-exact comparison against the real golden model
(lib/sync/bootstrap_detector.cc, ATSC3_FIXED_POINT=ON) via the
hdl/sim/golden/bootstrap_gen CLI -- never a Python reimplementation. Both
sides consume the same in-memory stimulus, generated once here.

Two things are compared:
  * every processed sample's monitor output (smoothed metric and current
    CFO, i.e. get_current_metric()/get_current_cfo_hz()), which checks the
    whole datapath each sample rather than only at detection instants;
  * every BootstrapDetection status word (sample index, CFO, peak metric).

Stimulus is synthetic Schmidl-Cox bootstrap-like symbols (one random
half-symbol of L = 2048 samples, repeated) at known CFO offsets inside
the unambiguous range +-Fs/(2L), in noise, followed by silence and
full-scale bursts to exercise the metric-saturation and normalization-shift
corners. The input is driven with random valid gaps and the status output
with random ready backpressure.

Run via test_runner.py (pytest), not directly.
"""

import cmath
import math
import random
import subprocess
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import FallingEdge, RisingEdge, with_timeout

REPO_ROOT = Path(__file__).resolve().parents[3]
BOOTSTRAP_GEN = REPO_ROOT / "build-fxp" / "hdl" / "sim" / "golden" / "bootstrap_gen"

HALF_SYMBOL = 2048
MAX_WIN = 1024  # bootstrap_detector.v's default 2^MAX_WIN_LOG2
DEFAULT_FS = 6_250_000
DEFAULT_THRESHOLD_Q15 = 22937  # float_to_q15(0.7f), the register reset value

# status_words.vh BootstrapDetection layout
DET_SAMPLE_INDEX = (8, 48)
DET_CFO_HZ = (56, 32)
DET_METRIC = (88, 32)


def _field(word, lsb_width, signed=False):
    lsb, width = lsb_width
    v = (word >> lsb) & ((1 << width) - 1)
    if signed and v & (1 << (width - 1)):
        v -= 1 << width
    return v


def _clamp16(v):
    return max(-32768, min(32767, int(round(v))))


def noise(rnd, n, sigma):
    return [(_clamp16(rnd.gauss(0, sigma)), _clamp16(rnd.gauss(0, sigma))) for _ in range(n)]


def bootstrap(rnd, cfo_hz, fs, amplitude, noise_sigma):
    """One half-symbol of complex Gaussian, repeated, rotated by the CFO."""
    half = [complex(rnd.gauss(0, 1), rnd.gauss(0, 1)) for _ in range(HALF_SYMBOL)]
    out = []
    for n, s in enumerate(half + half):
        v = s * amplitude * cmath.exp(2j * math.pi * cfo_hz * n / fs)
        out.append((_clamp16(v.real + rnd.gauss(0, noise_sigma)),
                    _clamp16(v.imag + rnd.gauss(0, noise_sigma))))
    return out


def full_scale_burst(rnd, n):
    """Includes -32768 on both rails, the corner the C++ products are sized for."""
    corners = (-32768, 32767)
    return [(rnd.choice(corners), rnd.choice(corners)) for _ in range(n)]


def decaying_tail(rnd, n, amplitude, tau):
    """Noise whose amplitude decays as exp(-t/tau) until it rounds to zero."""
    return noise_samples(rnd, [amplitude * math.exp(-t / tau) for t in range(n)])


def noise_samples(rnd, sigmas):
    return [(_clamp16(rnd.gauss(0, s)), _clamp16(rnd.gauss(0, s))) for s in sigmas]


def run_golden(cfg, samples):
    if not BOOTSTRAP_GEN.exists():
        raise FileNotFoundError(
            f"{BOOTSTRAP_GEN} not found -- build it first: "
            "cmake --build build-fxp --target bootstrap_gen "
            "(requires -DATSC3_FIXED_POINT=ON -DATSC3_ENABLE_HDL_STUBS=ON)"
        )
    fs, thr, win = cfg
    lines = [f"C {fs} {thr} {win}"] + [f"{re} {im}" for re, im in samples]
    proc = subprocess.run([str(BOOTSTRAP_GEN)], input="\n".join(lines) + "\n",
                          capture_output=True, text=True, check=True)
    mon, det = [], []
    for line in proc.stdout.splitlines():
        tag, *vals = line.split()
        if tag == "M":
            mon.append(tuple(int(v) for v in vals))
        elif tag == "D":
            det.append(tuple(int(v) for v in vals))
        else:
            raise ValueError(f"bad golden line: {line}")
    assert len(mon) == len(samples), (
        f"golden CLI returned {len(mon)} monitor lines for {len(samples)} samples")
    return mon, det


async def reset_dut(dut, cfg):
    fs, thr, win = cfg
    dut.rst.value = 1
    dut.cfg_sample_rate_hz.value = fs
    dut.cfg_threshold_q15.value = thr & 0xFFFF
    dut.cfg_averaging_window.value = win
    dut.s_axis_tvalid.value = 0
    dut.s_axis_tdata.value = 0
    dut.s_axis_tlast.value = 0
    dut.m_axis_tready.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst.value = 0
    await RisingEdge(dut.clk)


async def drive(dut, samples, rnd):
    for re, im in samples:
        if rnd.random() < 0.05:  # occasional valid gap
            dut.s_axis_tvalid.value = 0
            for _ in range(rnd.randint(1, 4)):
                await RisingEdge(dut.clk)
        dut.s_axis_tdata.value = ((re & 0xFFFF) << 16) | (im & 0xFFFF)
        dut.s_axis_tvalid.value = 1
        while True:
            await FallingEdge(dut.clk)
            if dut.s_axis_tready.value == 1:
                break
            await RisingEdge(dut.s_axis_tready)
        await RisingEdge(dut.clk)  # handshake edge
        dut.s_axis_tvalid.value = 0


async def collect_monitor(dut, out):
    while True:
        await RisingEdge(dut.mon_valid)
        out.append((int(dut.mon_metric.value), dut.mon_cfo_hz.value.signed_integer))


async def collect_detections(dut, out, rnd):
    while True:
        await RisingEdge(dut.m_axis_tvalid)
        word = int(dut.m_axis_tdata.value)
        assert dut.m_axis_tlast.value == 1, "status word must carry TLAST"
        for _ in range(rnd.randint(0, 6)):  # backpressure
            await RisingEdge(dut.clk)
            assert dut.m_axis_tvalid.value == 1, "m_axis_tvalid dropped before handshake"
            assert int(dut.m_axis_tdata.value) == word, "m_axis_tdata changed while stalled"
            assert dut.s_axis_tready.value == 0, "input accepted while a detection is pending"
        await FallingEdge(dut.clk)
        dut.m_axis_tready.value = 1
        await RisingEdge(dut.clk)  # handshake edge
        dut.m_axis_tready.value = 0
        assert word & 1, "detected bit not set"
        out.append((_field(word, DET_SAMPLE_INDEX),
                    _field(word, DET_CFO_HZ, signed=True),
                    _field(word, DET_METRIC)))


async def run_scenario(dut, cfg, samples, seed, expect_detections, check_golden=None):
    golden_mon, golden_det = run_golden(cfg, samples)
    if check_golden:
        check_golden(golden_mon)
    if expect_detections:
        # Guards against a vacuous run: the stimulus must actually trigger
        # the golden model, or matching "no detections" proves little.
        assert golden_det, "stimulus produced no golden detections"
    dut._log.info("golden detections (index, cfo_hz, metric): %s", golden_det)

    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    await reset_dut(dut, cfg)

    rnd = random.Random(seed)
    mon, det = [], []
    cocotb.start_soon(collect_monitor(dut, mon))
    cocotb.start_soon(collect_detections(dut, det, random.Random(seed + 1)))

    # ~170 cycles/sample plus the RAM clear sweep and gaps; generous margin.
    budget_ns = (len(samples) * 400 + 4 * HALF_SYMBOL) * 10
    await with_timeout(drive(dut, samples, rnd), budget_ns, "ns")
    # Let the last sample's pipeline and any final detection drain.
    for _ in range(400):
        await RisingEdge(dut.clk)

    assert len(mon) == len(golden_mon), (
        f"monitor produced {len(mon)} samples, golden {len(golden_mon)}")
    for i, (got, exp) in enumerate(zip(mon, golden_mon)):
        assert got == exp, (
            f"sample {i + 1}: (smoothed_metric, cfo_hz) expected {exp}, got {got}")
    assert det == golden_det, f"detections differ:\n  rtl    {det}\n  golden {golden_det}"


@cocotb.test()
async def two_bootstraps_default_config(dut):
    """Register-reset config; two bootstraps at opposite CFOs, then silence
    (drives R to its floor while P decays -- the metric-saturation path)."""
    rnd = random.Random(0xB0075)
    fs = DEFAULT_FS
    amp, sigma = 6000.0, 600.0  # ~20 dB SNR
    samples = (noise(rnd, 300, sigma)
               + bootstrap(rnd, 800.0, fs, amp, sigma)
               + noise(rnd, 1500, sigma)
               + bootstrap(rnd, -1200.0, fs, amp, sigma)
               + [(0, 0)] * 2500)
    await run_scenario(dut, (fs, DEFAULT_THRESHOLD_Q15, 64), samples, 11, True)


@cocotb.test()
async def raw_angle_odd_window(dut):
    """Fs = 2^27 makes cfo_hz == the raw CORDIC angle, exposing it bit-for-bit;
    window 5 exercises a non-power-of-two average divide. Ends with
    full-scale corner samples into silence."""
    rnd = random.Random(0x5A5A)
    fs = 1 << 27
    amp, sigma = 9000.0, 300.0
    samples = (noise(rnd, 100, sigma)
               # unambiguous range at Fs = 2^27 is +-Fs/(2L) = +-32768 Hz;
               # 20 kHz is a phase of ~0.61*pi
               + bootstrap(rnd, 20000.0, fs, amp, sigma)
               + full_scale_burst(rnd, 300)
               + [(0, 0)] * 1200)
    await run_scenario(dut, (fs, 16384, 5), samples, 22, True)


@cocotb.test()
async def metric_saturation_boundary(dut):
    """A bootstrap fading out slowly into silence: R shrinks smoothly as the
    fade passes through the delay line while P decays, so |P|/R sweeps
    through the metric-saturation boundary (magnitude 2^23) instead of
    jumping past it. Window 1 makes each sample's metric directly visible."""
    rnd = random.Random(0xFADE)
    amp = 6000.0
    samples = (bootstrap(rnd, 500.0, DEFAULT_FS, amp, 0.0)
               + decaying_tail(rnd, 6500, amp, 300.0))

    def covers_boundary(golden_mon):
        metrics = [m for m, _ in golden_mon]
        # just below saturation: magnitude in [2^22, 2^23)
        assert any((1 << 29) <= m < 0x7FFFFFFF for m in metrics), (
            "stimulus never approaches the saturation boundary")
        assert 0x7FFFFFFF in metrics, "stimulus never saturates the metric"

    await run_scenario(dut, (DEFAULT_FS, DEFAULT_THRESHOLD_Q15, 1), samples, 44, True,
                       covers_boundary)


@cocotb.test()
async def zero_window_clamps_to_one(dut):
    """averaging_window = 0 behaves as 1 (the golden model's own clamp) and
    does not raise cfg_window_clamped, which is reserved for the RTL-only
    upper bound."""
    rnd = random.Random(0x0)
    samples = noise(rnd, 200, 500.0) + bootstrap(rnd, -400.0, DEFAULT_FS, 7000.0, 500.0)
    await run_scenario(dut, (DEFAULT_FS, DEFAULT_THRESHOLD_Q15, 0), samples, 33, True)
    assert dut.cfg_window_clamped.value == 0


@cocotb.test()
async def oversized_window_is_flagged(dut):
    """A window beyond the RTL history RAM depth is clamped and flagged --
    the one config where the RTL knowingly diverges from the C++."""
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    await reset_dut(dut, (DEFAULT_FS, DEFAULT_THRESHOLD_Q15, MAX_WIN + 1))
    assert dut.cfg_window_clamped.value == 1
    await reset_dut(dut, (DEFAULT_FS, DEFAULT_THRESHOLD_Q15, MAX_WIN))
    assert dut.cfg_window_clamped.value == 0
