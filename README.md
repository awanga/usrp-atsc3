# OpenATSC3 — ATSC 3.0 Software-Defined Receiver

[![CI](https://github.com/awanga/usrp-atsc3/actions/workflows/ci.yml/badge.svg)](https://github.com/awanga/usrp-atsc3/actions/workflows/ci.yml)

> A GNU Radio out-of-tree (OOT) module implementing a full ATSC 3.0 physical-layer
> receiver targeting the USRP N2x0 platform with TVRX daughterboard.
> Architected for GR-version agnosticism (3.8 / 3.10), FPGA portability, and
> post-MVP machine-learning-based multipath mitigation.

---

## Table of Contents

1. [Hardware Requirements](#hardware-requirements)
2. [Software Requirements](#software-requirements)
3. [Architecture Overview](#architecture-overview)
4. [Signal Chain](#signal-chain)
5. [Module Descriptions](#module-descriptions)
6. [Build Instructions](#build-instructions)
7. [Usage](#usage)
8. [Configuration Reference](#configuration-reference)
9. [CI/CD](#cicd)
10. [HDL Portability Design](#hdl-portability-design)
11. [Post-MVP Roadmap](#post-mvp-roadmap)
12. [References](#references)

---

## Hardware Requirements

| Component        | Specification                                          |
|------------------|--------------------------------------------------------|
| SDR Platform     | USRP N210 or N200                                      |
| Daughterboard    | TVRX (50–860 MHz, ~25 dB NF typical)                  |
| Host Interface   | Gigabit Ethernet (direct or switch)                    |
| UHD Version      | 3.15.x                                                 |
| Host CPU         | x86\_64, ≥4 cores recommended                         |
| Host RAM         | ≥8 GB (LDPC decoder is memory-intensive)               |
| GPU (optional)   | CUDA-capable, for post-MVP ML inference                |

**TVRX Tuning Range:** 50–860 MHz covers all US ATSC 3.0 broadcast channels
(UHF 14–69: 470–806 MHz; VHF 7–13: 174–216 MHz).

**Sample Rate:** 6.25 MS/s complex (minimum); 12.5 MS/s recommended for
guard-band headroom. N210 FPGA can sustain 25 MS/s over GigE.

---

## Software Requirements

### Required

| Package               | Version     | Notes                                  |
|-----------------------|-------------|----------------------------------------|
| GNU Radio             | 3.8.x or 3.10.x | Via HAL abstraction layer          |
| UHD                   | 3.15.x      | Paired with GR 3.8; GR 3.10 via shim  |
| CMake                 | ≥ 3.16      |                                        |
| GCC / Clang           | GCC ≥ 9, Clang ≥ 11 |                               |
| FFTW3                 | ≥ 3.3       | Optional, used when found (`-lfftw3f`); GPL, see `LICENSING.md` |
| Boost                 | ≥ 1.71      |                                        |
| FFmpeg / libav        | ≥ 4.x       | HEVC, AC-4, HE-AAC decode              |
| GStreamer             | ≥ 1.16      | Live A/V playback pipeline             |
| libgst-plugins-bad    | ≥ 1.16      | HEVC GStreamer elements                |
| Python                | ≥ 3.8       | Tooling, GRC, scanner CLI             |

### Optional / Post-MVP

| Package           | Purpose                                      |
|-------------------|----------------------------------------------|
| PyTorch ≥ 2.0     | ML multipath model training                  |
| ONNX Runtime      | Embedded inference (replaces PyTorch at RX)  |
| Verilator ≥ 4.x   | RTL simulation of HDL port                   |
| cocotb            | Python-based RTL testbench                   |
| lcov / gcov       | C++ code coverage                            |
| Doxygen           | API documentation                            |

---

## Architecture Overview

The project is organized into four layers with strict dependency isolation:

```
┌─────────────────────────────────────────────────────────────────┐
│                     Application Layer                           │
│   atsc3_rx (GRC flowgraph)   scanner CLI   metrics dashboard    │
└────────────────────────┬────────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────────┐
│                  GNU Radio Blocks Layer  (blocks/)              │
│  Thin GR wrappers — no DSP logic here, only buffer plumbing     │
│  All blocks expose AXI4-Stream-equivalent port contracts         │
└────────────────────────┬────────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────────┐
│                     Core DSP Library  (lib/)                    │
│  Pure C++17, no GNU Radio headers, no UHD headers               │
│  Compile-time FIXED_POINT mode (int16/int32) vs float32         │
│  Directly unit-testable and HDL-translatable                    │
│                                                                 │
│   sync/   ofdm/   channel/   fec/   framing/   metrics/        │
└────────────────────────┬────────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────────┐
│              Hardware Abstraction Layer  (hal/)                 │
│  IQSource interface — UHDSource | FileSource | NullSource       │
│  Isolates UHD version from all other code                       │
└─────────────────────────────────────────────────────────────────┘
```

### Dependency Rule (strictly enforced via CMake)

```
hal/  →  lib/  only
lib/  →  (Boost headers; FFTW3 when found)  only
blocks/  →  lib/ + gnuradio-runtime  only
apps/  →  blocks/ + hal/ + av/
av/  →  (FFmpeg, GStreamer)  only
ml/  →  lib/ + (ONNX Runtime)  only
```

No layer may import from a layer above it. Violations caught by `cmake --graphviz`.

---

## Signal Chain

ATSC 3.0 receive chain: the inverse of the A/322 transmitter (Figures 4.1,
6.1, 7.1), preceded by A/321 bootstrap acquisition. The blocks are
implemented and unit-tested but not yet joined into one receiver (see
`docs/compliance.md`).

```
RF In (6 MHz channel)
      │
      ▼
┌──────────────────┐   hal/IQSource
│  RF Frontend     │   set_freq(), set_gain(), set_rate(); AGC loop
│  (TVRX/UHD)      │   N210: 6.25 MS/s
└──────┬───────────┘
       ▼
┌──────────────────┐   lib/dsp/resampler
│  Resample        │   to the bootstrap rate, 6.144 MS/s (A/321 5.1)
└──────┬───────────┘
       ▼
┌──────────────────┐   lib/sync/bootstrap_*
│ Bootstrap detect │   2048-point FFT, 3072-sample CAB/BCA symbols;
│ + decode         │   timing, CFO, version 0.0 signaling fields
└──────┬───────────┘
       ▼
┌──────────────────┐   lib/sync/frame_sync
│  Frame sync      │   frame boundaries = validated bootstraps
└──────┬───────────┘
       ▼
┌──────────────────┐   lib/dsp/resampler
│  Resample        │   to (bsr_coefficient + 16) x 0.384 MS/s
│                  │   (6.912 MS/s for 6 MHz, A/322 9.1)
└──────┬───────────┘
       ▼
┌──────────────────┐   lib/ofdm/cp_removal, fft_engine
│  GI removal, FFT │   A/322 Table 8.9 guard intervals; 8K/16K/32K
└──────┬───────────┘
       ▼
┌──────────────────┐   lib/ofdm/pilot_extractor, lib/channel/
│ Channel estimate │   scattered/continual/edge pilots; LS + Wiener
│ + equalize       │   interpolation; single-tap equalizer
└──────┬───────────┘
       ▼
┌──────────────────┐   lib/ofdm/freq_deinterleaver
│ Freq de-interlv  │   A/322 7.3
└──────┬───────────┘
       ├───────────────────────────────┐
       ▼ preamble cells                ▼ data cells
┌──────────────────┐   lib/framing/   ┌──────────────────┐   lib/ofdm/
│ L1-Basic,        │   l1_fec,        │ Time de-interlv  │   time_ / cell_
│ L1-Detail decode │   l1_signaling   │ (CTI / HTI)      │   deinterleaver
└──────┬───────────┘                  └──────┬───────────┘
       │ configures every block              ▼
       │ after the bootstrap        ┌──────────────────┐   lib/ofdm/
       └──────────────────────────► │ Demap (QAM/NUC)  │   constellation_demapper
                                    │ soft LLRs        │
                                    └──────┬───────────┘
                                           ▼
                                    ┌──────────────────┐   lib/fec/
                                    │ Bit de-interlv   │   bit_deinterleaver
                                    │ LDPC (min-sum)   │   ldpc_decoder
                                    │ BCH / CRC-32     │   bch_decoder
                                    └──────┬───────────┘
                                           ▼
                                    ┌──────────────────┐   lib/framing/
                                    │ Baseband packets │   baseband_packet
                                    │ ALP demux        │   alp_demux
                                    │ ROUTE/DASH       │   route_parser
                                    └──────┬───────────┘
                                           ▼ HEVC / AC-4
                                    ┌──────────────────┐   av/
                                    │ FFmpeg decode    │
                                    │ GStreamer player │
                                    └──────────────────┘
```

### L1 Signaling

The bootstrap's last symbol signals the preamble structure (A/322 Table
H.1.1): FFT size, guard interval, pilot spacing and L1-Basic FEC mode of
the preamble. The preamble carries L1-Basic (fixed 200 bits) and
L1-Detail (variable), each protected by BCH + LDPC with shortening and
puncturing (A/322 Section 6.5). Their fields (A/322 Sections 9.2, 9.3)
configure everything after the preamble: frame length, subframes, FFT
size, guard interval, pilots, and per-PLP modulation, code rate and time
interleaving. `lib/framing/l1_fec` and `l1_signaling` implement both;
nothing yet feeds them preamble cells or applies their output.

---

## Module Descriptions

### `hal/` — Hardware Abstraction Layer

Defines `IQSource` pure-virtual interface:

```cpp
class IQSource {
public:
    virtual void   set_frequency(double hz)    = 0;
    virtual void   set_gain(double db)         = 0;
    virtual void   set_sample_rate(double sps) = 0;
    virtual size_t read(std::complex<float>* buf, size_t n) = 0;
    virtual double get_rssi_dbm()              = 0;  // from TVRX RSSI port
    virtual ~IQSource() = default;
};
```

Concrete implementations:
- `UHDSource` — wraps `uhd::usrp::multi_usrp`, version-gated via `UHD_VERSION`
  macros to support both UHD 3.15 and UHD 4.x APIs.
- `FileSource` — reads interleaved complex float32 from `.iq` file; used for
  CI replay tests.
- `NullSource` — returns zeros; used for unit tests of downstream blocks.

### `lib/sync/` — Synchronization

- `bootstrap_signal` — A/321 bootstrap sequences (Zadoff-Chu x PN), Gray
  demapping and signaling fields; A/322 preamble structure table.
- `bootstrap_detector` — Finds bootstrap symbol 0 from its CAB
  repetitions at 6.144 MS/s; outputs its start sample and fractional CFO.
- `bootstrap_decoder` — Correlates each symbol with its known sequence:
  timing, integer CFO, signaling fields, last-symbol check.
- `frame_sync` — Frame boundaries from validated bootstraps, checked
  against min_time_to_next and the L1 frame length; short holdover.
- `timing_recovery` — Gardner timing error detector; polyphase interpolator
  for fractional sample correction.

### `lib/dsp/` — Shared DSP Primitives

- `cordic` — Shift-add rotation and vectoring (NCO, magnitude/phase).
- `resampler` — Rational polyphase resampler (exact position accumulator,
  Kaiser windowed sinc) from the radio rate to 6.144 MS/s and to
  (bsr_coefficient + 16) x 0.384 MS/s.

### `lib/ofdm/` — OFDM Engine

- `cp_removal` — Strips the guard interval (A/322 Table 8.9 lengths,
  pattern from L1).
- `fft_engine` — FFTW3f when available, else a built-in radix-2 FFT
  (`-DATSC3_USE_FFTW=OFF` forces it); 2048 (bootstrap), 8192, 16384 and
  32768 point transforms.
  In `FIXED_POINT` mode, uses fixed-point Cooley-Tukey reference implementation
  for numerical equivalence testing against RTL.
- `pilot_extractor` — Extracts scattered pilots (SP), continual pilots (CP),
  and edge pilots per ATSC A/322 §8.1, Annex D (continual) and Annex E
  (sixteen scattered patterns SP3_2 to SP32_4), with carrier reduction.
- `constellation_demapper` — Produces soft LLRs; includes NUC lookup tables
  for all defined constellations (NUC tables generated from A/322 Annex C).
- `freq_deinterleaver`, `time_deinterleaver`, `cell_deinterleaver` —
  Inverses of the A/322 frequency interleaver (7.3), CTI and HTI time
  interleavers (7.1.4, 7.1.5; no inter-subframe CDL yet) and cell
  interleaver (7.1.5.2).

### `lib/channel/` — Channel Processing

- `channel_estimator` — LS estimate at pilot positions; 2D Wiener filter
  interpolation across time and frequency.
- `equalizer` — Single-tap FDE per subcarrier; phase noise tracker using
  continual pilots.

### `lib/fec/` — Forward Error Correction

- `ldpc_decoder` — Min-sum belief propagation decoder. Parity check matrices
  for all ATSC 3.0 code rates (2/15 through 13/15), built from the A/322
  Annex A tables and stored as sparse arrays.
- `bch_decoder` — A/322 outer codes: 12-error-correcting BCH over GF(2^16)
  (64800-bit LDPC) or GF(2^14) (16200-bit), and CRC-32.
- `bit_deinterleaver` — Inverse of the A/322 bit interleaver (6.2; parity,
  group-wise and block interleaving, Annex B tables).

### `lib/framing/` — Transport Framing

- `l1_fec` — L1-Basic and L1-Detail FEC decoding (A/322 6.5).
- `l1_signaling` — L1-Basic and L1-Detail field parsing (A/322 9.2, 9.3).
- `baseband_packet` — Baseband packet descrambling and header parsing
  (A/322 5.2).
- `alp_demux` — ALP (Application Link Protocol) header parsing and PLP
  demultiplexing.
- `route_parser` — ROUTE/DASH session announcement and segment reassembly.
- `service_catalog` — Maintains discovered service list (SLT from ROUTE).

### `lib/metrics/` — Signal Quality

- `snr_estimator` — Decision-directed SNR from pilot residuals.
- `mer_estimator` — Modulation Error Ratio from equalized constellation.
- `ber_estimator` — Pre-FEC BER from LDPC iteration count vs threshold.
- `signal_strength` — RSSI from HAL; AGC gain-backed dBm estimate.

### `blocks/` — GNU Radio OOT Blocks

Thin wrapper blocks; each delegates immediately to the corresponding `lib/`
class. No DSP logic resides here. AXI4-Stream port contracts are documented
in each block's header as:

```
// AXI4-S contract: TDATA=cf32, TVALID, TREADY, TLAST (frame boundary)
```

### `av/` — Audio/Video Integration

- `hevc_decoder` — FFmpeg-backed HEVC ES decoder; outputs raw YUV frames.
- `ac4_decoder` — FFmpeg-backed AC-4 / HE-AAC audio decoder; outputs PCM.
- `gst_player` — GStreamer pipeline; connects `appsrc` to `autovideosink` /
  `autoaudiosink`. Handles A/V sync via GStreamer clock.

### `apps/` — Applications

- `atsc3_rx.grc` — GRC flowgraph; USRP source → full decode pipeline → metrics
  display. Parameters: center frequency, gain.
- `scanner.py` — CLI channel scanner; sweeps ATSC 3.0 channel plan, reports
  signal strength, lock status, and detected services per channel.
- `metrics_server.py` — Lightweight HTTP server exposing JSON metrics endpoint
  for dashboard integration.

### `hdl/` — RTL Port (Post-MVP)

Contains port-map documentation, AXI4-Stream interface definitions, and
Verilator simulation harnesses for each `lib/` block. RTL is technology-
agnostic: no vendor primitives, no IP cores. DSP48-friendly arithmetic patterns
are documented but not vendor-locked. Target: any FPGA with DSP slices
≥ 18×18 multipliers.

### `ml/` — Machine Learning Multipath Mitigation (Post-MVP)

- `data/` — IQ capture datasets and channel simulation scripts (ray-tracing
  and stochastic multipath models).
- `models/` — PyTorch model definitions: CNN/LSTM channel estimator to replace
  the Wiener filter equalizer; trained offline.
- `inference/` — ONNX Runtime C++ inference wrapper; drops in as an alternative
  `channel_estimator` implementation with identical interface.

---

## Build Instructions

```bash
# 1. Install system dependencies (Ubuntu 22.04)
sudo apt-get install -y \
    cmake build-essential libboost-all-dev libfftw3-dev \
    libuhd-dev gnuradio-dev \
    ffmpeg libavcodec-dev libavformat-dev libavutil-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev gstreamer1.0-plugins-bad \
    python3-dev python3-numpy

# 2. Clone and configure
git clone https://github.com/<org>/OpenATSC3.git
cd OpenATSC3
mkdir build && cd build

# 3. Configure
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DATSC3_FIXED_POINT=OFF \
    -DATSC3_ENABLE_ML=OFF \
    -DATSC3_ENABLE_HDL_STUBS=OFF

# 4. Build
cmake --build . -j$(nproc)

# 5. Install (or use without installing via PYTHONPATH / GRC_BLOCKS_PATH)
sudo cmake --install .

# Optional: Fixed-point simulation build
cmake .. -DATSC3_FIXED_POINT=ON
cmake --build . -j$(nproc) --target atsc3_fxp_tests
```

### CMake Options

| Option                   | Default | Description                                  |
|--------------------------|---------|----------------------------------------------|
| `ATSC3_FIXED_POINT`      | OFF     | Build lib/ with int16/int32 fixed-point      |
| `ATSC3_ENABLE_ML`        | OFF     | Build ml/ inference wrapper (needs ONNX RT)  |
| `ATSC3_ENABLE_HDL_STUBS` | OFF     | Build hdl/ Verilator simulation targets      |
| `ATSC3_BUILD_TESTS`      | ON      | Build unit and integration tests             |
| `ATSC3_GR_VERSION`       | auto    | Override GR version detection (38 or 310)    |
| `ATSC3_CHANNEL_BW_HZ`    | 6000000 | Channel bandwidth; 6/7/8 MHz for future use  |

---

## Usage

### Receive a Channel

```bash
# Tune to ATSC 3.0 channel 28 (557 MHz center)
gnuradio-companion apps/atsc3_rx.grc
# Or headless:
python3 apps/atsc3_rx.py --freq 557e6 --gain 30
```

### Scan for Channels

```bash
# Full UHF scan (channels 14–51)
python3 apps/scanner.py --band uhf

# Single channel probe
python3 apps/scanner.py --channel 28

# Output JSON
python3 apps/scanner.py --band uhf --format json > channels.json
```

Scanner output columns: Channel | Frequency | RSSI (dBm) | Lock | MER (dB) | Services

### Run Tests

```bash
cd build
ctest --output-on-failure -j$(nproc)

# Unit tests only (no hardware needed)
ctest -L unit

# Integration tests (uses IQ replay; no hardware needed)
ctest -L integration
```

---

## Configuration Reference

Channel plan (`config/channel_plan_us.json`):
```json
{
  "region": "US",
  "bandwidth_hz": 6000000,
  "channels": [
    { "number": 14, "center_hz": 473000000 },
    { "number": 28, "center_hz": 557000000 }
  ]
}
```

Multi-region bandwidth is selected at CMake time via `ATSC3_CHANNEL_BW_HZ`.
The channel plan JSON is the only region-specific runtime artifact.

---

## CI/CD

**Platform:** GitHub Actions

### Workflows

#### `ci.yml` — Pull Request Gate (every PR/push)
- Ubuntu 22.04 runner
- Installs GNU Radio 3.8 from PPA, UHD 3.15
- Builds with `-DATSC3_BUILD_TESTS=ON`
- Runs all unit tests (`ctest -L unit`)
- Runs integration tests against pre-recorded IQ captures (stored in `test/captures/` via **Git LFS**)
- Runs `clang-format --dry-run` check
- Runs `cppcheck` static analysis
- Required to pass before merge

#### `nightly.yml` — Nightly Extended Suite
- Runs full integration test suite including long captures (>1 min IQ files)
- Also builds and tests against GNU Radio 3.10 (compatibility shim verification)
- Generates code coverage report (lcov), uploads to Codecov
- Builds Doxygen documentation, deploys to GitHub Pages on `main`

#### `release.yml` — Tag-triggered Release
- Builds release artifacts (Debian `.deb` package via `cpack`)
- Runs full test suite
- Creates GitHub Release with changelog and `.deb` artifact

### Test Strategy

| Test Type        | Location          | Hardware | IQ Needed  | CI Stage    |
|------------------|-------------------|----------|------------|-------------|
| Unit             | `test/unit/`      | None     | None       | PR gate     |
| DSP numerical    | `test/unit/`      | None     | Generated  | PR gate     |
| Integration      | `test/integration/`| None    | Git LFS    | PR gate     |
| Long-form replay | `test/integration/`| None    | Git LFS    | Nightly     |
| Live hardware    | `test/hw/`        | USRP N2x0| Live RF   | Manual only |

**IQ test vector policy:** Captures stored in `test/captures/` via Git LFS.
Minimum set: one bootstrap-locked 10-second capture per supported FFT size
(8K, 16K, 32K). Files must be anonymized (no PII in decoded stream).

### Docker

`docker/Dockerfile.ci` provides a reproducible build environment:

```dockerfile
FROM ubuntu:22.04
RUN apt-get install -y gnuradio uhd-host libfftw3-dev ...
# Pinned package versions in docker/versions.lock
```

Used locally via `make docker-build` and by GitHub Actions via
`docker://ghcr.io/<org>/openatsc3-ci:latest`.

---

## HDL Portability Design

Every `lib/` DSP block is written to satisfy these constraints simultaneously:

1. **AXI4-Stream contract** — Each block's `process()` function maps 1:1 to an
   AXI4-Stream slave/master pair. The C++ signature mirrors the port signals:
   `process(in_tdata, in_tvalid, in_tlast, out_tdata, out_tvalid, out_tready)`

2. **No dynamic allocation in steady state** — All buffers pre-allocated at
   init. RTL has no heap.

3. **Fixed-point equivalence tests** — `ATSC3_FIXED_POINT=ON` builds the same
   block with `int16_t` intermediate values and Q-format specified per block.
   Output must match the float build within a defined SNR threshold
   (≥ 40 dB by default).

4. **No vendor DSP primitives** — FFTW3 is used at the C++ level; the
   `hdl/` FFT is a parameterized Cooley-Tukey butterfly implementation.
   The LDPC decoder uses only add/compare operations (min-sum), mapping
   cleanly to DSP slices.

5. **Parameter tables, not hardcoded constants** — All ATSC 3.0 mode tables
   (LDPC H matrices, pilot patterns, CP fractions) are loaded from JSON at
   runtime in C++ and will be ROM in RTL.

The `hdl/stubs/` directory contains AXI4-S Verilog interface definitions and
a `README` specifying the Q-format, clock domain, and pipeline latency for each
block. Verilator + cocotb testbenches will be added incrementally post-MVP.

---

## Post-MVP Roadmap

| Phase       | Feature                                             |
|-------------|-----------------------------------------------------|
| MVP+1       | ML channel estimator (CNN, offline-trained, ONNX)   |
| MVP+2       | Turbo/ML equalizer for multipath-heavy channels      |
| MVP+3       | Verilator RTL simulation of OFDM + FEC pipeline     |
| MVP+4       | Full RTL port: LDPC decoder + channel estimator     |
| MVP+5       | AXI4-Lite control plane for all RTL blocks          |
| MVP+6       | FPGA integration test (board TBD; no vendor IPs)    |
| Future      | SFN (single frequency network) support              |
| Future      | A/V1 codec support (AV1 decode)                     |
| Future      | ATSC 3.0 ROUTE/NRT service recording                |

---

## References

- **ATSC A/322:2017** — Physical Layer Protocol (ATSC 3.0 standard)
- **ATSC A/330** — Link-Layer Protocol
- **ATSC A/337** — Application Signaling
- **ATSC A/338** — Layered Coding Transport
- **GNU Radio OOT Module Guide** — https://wiki.gnuradio.org/index.php/OutOfTreeModules
- **UHD 3.15 Manual** — https://files.ettus.com/manual/
- **FFTW3 Documentation** — https://www.fftw.org/fftw3_doc/
- **DVB-T2 Framing Reference** (shares OFDM/LDPC structure) — ETSI EN 302 755
- **Min-Sum LDPC** — Richardson & Urbanke, "Modern Coding Theory", Cambridge 2008
- **Wiener Channel Estimation** — Edfors et al., IEEE Trans. Comm. 1998
- **ONNX Runtime C++ API** — https://onnxruntime.ai/docs/api/c/

---

## License

MPL-2.0 for software, CERN-OHL-W-2.0 for HDL design sources, CC-BY-4.0
for documentation. See [`LICENSING.md`](LICENSING.md) for the per-path
mapping and for which builds become GPL by linking GNU Radio, UHD or FFTW.
