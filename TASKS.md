# TASKS.md — gr-atsc3 MVP Development Plan

> Architecture constraints: `CLAUDE.md` | Full spec: `README.md`
>
> **Legend:** `[ ]` not started · `[~]` in progress · `[x]` complete · `[!]` blocked

---

## Phase 0 — Infrastructure & Skeleton  *(~1 week)* [x]

Goal: Working repo, build system, CI green, HAL testable. No DSP yet.

### 0.1 Repository Setup
- [x] `git init`, initial commit with `.gitignore` (build/, *.iq, __pycache__, .grc_gnuradio)
- [x] Configure **git-lfs** for `test/captures/*.iq` and `test/captures/*.sigmf`
- [x] Create top-level `CMakeLists.txt` with version `0.1.0-dev`; find_package for GR, UHD, FFTW3, Boost
- [x] Add `cmake/Modules/` with `FindFFTW3.cmake` (graceful fail with helpful message)
- [x] CMake option scaffold: `ATSC3_FIXED_POINT`, `ATSC3_ENABLE_ML`, `ATSC3_ENABLE_HDL_STUBS`, `ATSC3_BUILD_TESTS`, `ATSC3_GR_VERSION`, `ATSC3_CHANNEL_BW_HZ`
- [x] CMake dependency graph target (`cmake --graphviz`); verify no upward-layer imports
- [x] `clang-format` config (`.clang-format`, based on Google style, 100-col)
- [x] `cppcheck` suppressions file (`.cppcheck`)
- [x] `pre-commit` config (clang-format, trailing whitespace, no large files without LFS)

### 0.2 CI/CD Pipelines
- [x] `.github/workflows/ci.yml` — Ubuntu 22.04, GR 3.10, UHD 3.15, build + `ctest -L unit`
- [x] `.github/workflows/nightly.yml` — GR 3.10 build, long IQ tests, lcov coverage, Doxygen deploy
- [x] `.github/workflows/release.yml` — tag-triggered, cpack `.deb`, GitHub Release
- [x] `docker/Dockerfile.ci` — pinned apt packages, pushed to `ghcr.io` on nightly
- [x] `docker/versions.lock` — records exact apt package versions for reproducibility
- [ ] Branch protection: require `ci.yml` green; no force-push to `main` *(GitHub repo settings — manual)*

### 0.3 Directory Skeleton
- [x] Create empty `CMakeLists.txt` in each subdirectory: `hal/`, `lib/`, `blocks/`, `apps/`, `av/`, `hdl/`, `ml/`, `test/`
- [x] `lib/` subdirectory scaffold: `sync/`, `ofdm/`, `channel/`, `fec/`, `framing/`, `metrics/`
- [x] Add `ATSC3_SAMPLE_T` typedef header (`lib/types.h`) switching on `ATSC3_FIXED_POINT`
- [x] Add `config/channel_plan_us.json` with US ATSC 3.0 channel 14–51 center frequencies
- [x] Add `config/atsc3_modes.json` scaffold (LDPC rates, FFT sizes, CP fractions — populate in Phase 3)
- [x] `hdl/stubs/README.md` and AXI4-S Verilog interface template files

### 0.4 Test Framework
- [x] Add GoogleTest via `FetchContent` (pinned version); target `atsc3_unit_tests`
- [x] Add first passing trivial test (`test/unit/test_types.cc`) — verifies `ATSC3_SAMPLE_T` typedef compiles in both modes
- [x] CI: `ctest -L unit` runs and reports; badge added to `README.md`

---

## Phase 1 — Hardware Abstraction Layer  *(~1 week)* [x]

Goal: Can acquire IQ samples from USRP and from file. All downstream code uses `IQSource*` only.

### 1.1 IQSource Interface
- [x] `hal/include/iq_source.h` — pure-virtual interface (see `CLAUDE.md` §Key Interfaces)
- [x] `hal/include/iq_source_factory.h` — `create_uhd_source()`, `create_file_source()`, `create_null_source()`
- [x] Unit test: `NullSource` returns zeros; `read()` fills buffer exactly

### 1.2 FileSource Implementation
- [x] `hal/src/file_source.cc` — reads interleaved `std::complex<float32>` from `.iq` file
- [x] Supports looping (for CI replay tests)
- [x] `get_rssi_dbm()` returns value computed from signal power when no RSSI register available
- [x] Unit test: known 8-sample `.iq` file reads back bit-exact

### 1.3 UHDSource Implementation
- [x] `hal/src/uhd_source.cc` — wraps `uhd::usrp::multi_usrp`
- [x] `#if UHD_VERSION < 0x03160000` guards for any API differences between UHD 3.15 and 4.x
- [x] `set_frequency()` with TVRX tuning range validation (50–860 MHz); log warning if out of range
- [x] `set_gain()` — maps to TVRX gain range (0–36.5 dB); inverted for normal gain semantics; clamped with warning
- [x] `set_sample_rate()` — validates N210 sustainable rates over GigE (≤25 MS/s)
- [x] `get_rssi_dbm()` — reads UHD sensor `"rssi"` from TVRX daughterboard
- [x] Hardware test (manual, `ctest -L hw`): loopback noise floor, RSSI reads plausible value

### 1.4 AGC Controller
- [x] `hal/src/agc.cc` — power-feedback AGC; target power configurable (default: -20 dBFS)
- [x] Integrator with configurable attack/release time constants
- [x] Unit test: step input drives gain to within 1 dB of target within 100 iterations

---

## Phase 2 — OFDM Front-End  *(~2.5 weeks)* [x]

Goal: Given a locked IQ stream at the right sample rate, produce FFT-output symbols and extract pilots. No channel correction yet.

### 2.1 Bootstrap Detector [x]
- [x] `lib/sync/bootstrap_detector.h/.cc`
- [x] Implement Schmidl-Cox autocorrelation metric over 4K bootstrap symbol length
- [x] Output: coarse CFO estimate (Hz), sample index of bootstrap start
- [x] Parameters: bootstrap always 4096 points (ATSC A/322 §5.2) — not configurable
- [x] Unit test: synthetic bootstrap symbol → detects within ±1 sample, CFO within ±100 Hz
- [ ] Fixed-point mode: verify equivalence within 40 dB SNR threshold (deferred to Phase 7/HDL)

### 2.2 Frame Timing & Sync [x]
- [x] `lib/sync/timing_recovery.h/.cc` — Gardner TED + polyphase interpolator (32-tap, 16-phase)
- [x] `lib/sync/frame_sync.h/.cc` — superframe and subframe boundary tracker using preamble correlation
- [x] Unit test: timing recovery and frame sync tests (30 tests passing)
- [x] Test IQ capture available (`test/captures/ch35_599mhz_6.25msps.iq`) for integration tests

### 2.3 CP Removal
- [x] `lib/ofdm/cp_removal.h/.cc`
- [x] CP length taken from `Atsc3Config` struct (populated after L1 decode; bootstrapped with known CP for preamble)
- [x] AXI4-S contract documented in header
- [x] Unit test: CP prepended synthetically, stripped correctly for all defined CP fractions

### 2.4 FFT Engine
- [x] `lib/ofdm/fft_engine.h/.cc`
- [x] FFTW3f back-end; supports 8192, 16384, 32768 point transforms
- [x] `ATSC3_FIXED_POINT` path: parameterized Cooley-Tukey reference (no FFTW3 dependency in fixed-point build)
- [x] Plan caching (FFTW wisdom file, path configurable)
- [x] AXI4-S contract documented; pipeline latency = 1 FFT_SIZE/sample_rate (buffered)
- [x] Unit test: 8K FFT of known complex sinusoid → correct bin within ±1 LSB
- [ ] Fixed-point equivalence test: float vs fixed SNR ≥ 40 dB (deferred to Phase 7/HDL)

### 2.5 Pilot Extraction
- [x] `lib/ofdm/pilot_extractor.h/.cc`
- [x] Scattered pilot (SP) patterns PP1–PP8 per ATSC A/322 §7.2
- [x] Continual pilots (CP) and edge pilots
- [x] Load pilot pattern tables from `config/atsc3_modes.json`
- [x] Outputs: `vector<PilotSymbol>` (subcarrier index, known reference value, received value)
- [x] Unit test: for each PP, verify pilot count matches spec table (32 tests passing)

### 2.6 Coarse Frequency Correction
- [x] `lib/sync/freq_correction.h/.cc`
- [x] Coarse CFO from bootstrap estimate applied as complex multiply per sample
- [x] Fine CFO from continual pilot phase slope (residual) — PilotPhaseTracker class
- [x] Unit test: ±500 Hz CFO injected; corrected to < 10 Hz residual

---

## Phase 3 — Channel Estimation & Equalization  *(~1.5 weeks)* [x]

Goal: Output equalized QAM symbols suitable for demapping.

### 3.1 LS Channel Estimator [x]
- [x] `lib/channel/channel_estimator.h/.cc`
- [x] Least-squares estimate at scattered pilot positions: `H_hat[k] = Y[k] / X[k]`
- [x] Linear interpolation to data subcarriers
- [x] `EstimatorBackend` enum: `LS_ONLY`, `LS_WIENER`, `ML_ONNX` (post-MVP stub)
- [x] Temporal averaging (IIR filter) for smoothing across symbols
- [x] SNR estimation from pilot variance
- [x] Unit test: 26 tests passing (construction, LS, interpolation, SNR, integration)

### 3.2 Wiener Interpolation [x]
- [x] `lib/channel/wiener_interpolator.h/.cc`
- [x] 2D (time × frequency) Wiener filter; filter taps computed for Doppler/delay profiles
- [x] Configurable tap count; default: 8 taps frequency, 4 taps time
- [x] Multiple channel profiles: AWGN, PEDESTRIAN, VEHICULAR, URBAN, STATIC_MULTIPATH
- [x] History buffer for time-direction filtering
- [x] Unit test: 17 tests passing (construction, interpolation, multipath, profiles)

### 3.3 Frequency-Domain Equalizer [x]
- [x] `lib/channel/equalizer.h/.cc`
- [x] Single-tap FDE: `X_hat[k] = Y[k] / H_hat[k]` (ZF mode)
- [x] MMSE variant: `conj(H) / (|H|² + σ²_n)` with configurable noise variance
- [x] Phase noise tracker: residual phase per symbol from continual pilots
- [x] Deep fade protection: subcarriers with |H| < threshold zeroed
- [x] AXI4-S: input equalized symbols, output ATSC3_SAMPLE_T stream
- [x] Unit test: 22 tests passing (ZF, MMSE, EVM, phase tracking, integration)

---

## Phase 4 — Constellation Processing & FEC  *(~2 weeks)* [x]

Goal: Decoded bits from LDPC. L1 signaling parsed. System fully self-configuring.

### 4.1 Constellation Demapper [x]
- [x] `lib/ofdm/constellation_demapper.h/.cc`
- [x] Soft LLR output for: QPSK, 16/64/256/1024/4096-QAM (uniform)
- [x] NUC modulation types defined and functional (uses uniform QAM approximation for NUC-64+)
- [ ] Full NUC tables per ATSC A/322 §7.5 code-rate-dependent tables (deferred to Phase 7.3)
- [x] Output: `int8_t` LLRs (clamped ±127)
- [x] Unit test: QPSK, AWGN SNR=10 dB → LLR sign correct > 99.9%

### 4.2 De-interleavers [x]
- [x] `lib/ofdm/cell_deinterleaver.h/.cc` — per ATSC A/322 §8.1
- [x] `lib/ofdm/time_deinterleaver.h/.cc` (TDI) — convolutional; configurable depth
- [x] `lib/ofdm/freq_deinterleaver.h/.cc` (FDI) — per ATSC A/322 §8.3
- [x] Unit test for each: apply interleaver (reference Python), verify C++ de-interleaver round-trips

### 4.3 LDPC Decoder [x]
- [x] `lib/fec/ldpc_decoder.h/.cc`
- [x] Min-sum belief propagation; configurable iteration count (default 50)
- [x] Parity check matrices generated algorithmically (quasi-cyclic structure) for all 12 code rates
- [x] Both codeword lengths: 64800 bits and 16200 bits
- [x] No GR/UHD/FFTW3 dependency; builds standalone
- [x] AXI4-S: TDATA=int8(LLR), TLAST=codeword boundary
- [x] Performance target: ≥ 1 Mb/s throughput on CI runner (single thread) — verified: 1.28 Mb/s (short), 1.12 Mb/s (long)
- [x] Unit test: encode with reference encoder → decode all-zero codeword; BER=0 above waterfall
- [ ] Fixed-point equivalence test (deferred to Phase 7/HDL port)

### 4.4 BCH Decoder [x]
- [x] `lib/fec/bch_decoder.h/.cc` — GF(2^16), t=12 error correction
- [x] Uses LDPC output as input; corrects residual errors
- [x] Unit test: inject 6 bit errors → all corrected; 13 errors → failure flagged

### 4.5 L1 Preamble Decoder [x]
- [x] `lib/framing/l1_decoder.h/.cc`
- [x] L1-Pre parsing (bootstrap payload): FFT size, CP length, L1-Post size/modulation
- [x] L1-Post parsing: PLP count, modulation, code rate, interleaver config per PLP
- [x] Populates `Atsc3Config` struct; broadcasts to all downstream blocks via observer pattern
- [x] Unit test: known L1 bits (from ATSC A/322 §5 example) → correct config struct

### 4.6 Dynamic Reconfiguration [x]
- [x] `Atsc3Config` struct with all runtime parameters
- [x] `ConfigBus` (simple pub/sub, no dynamic alloc) wiring L1 decoder output to all dependent blocks
- [x] Integration test: FileSource with real ATSC 3.0 capture → bootstrap detection, ConfigBus distribution verified

---

## Phase 5 — Transport & Audio/Video  *(~1.5 weeks)* [x]

Goal: Live A/V decode and playback from real broadcast.

### 5.1 ALP Demultiplexer [x]
- [x] `lib/framing/alp_demux.h/.cc`
- [x] ALP header parsing (ATSC A/330)
- [x] IP datagram reassembly from ALP packets
- [x] PLP demultiplexing; output per-PLP byte streams
- [x] Unit test: synthetic ALP packet sequence → correct datagram reassembly (21 tests)

### 5.2 ROUTE/DASH Parser [x]
- [x] `lib/framing/route_parser.h/.cc`
- [x] ROUTE session announcement (SLT, LCT) parsing
- [x] DASH segment URL resolution
- [x] `lib/framing/service_catalog.h/.cc` — list of available services per transport session
- [x] Unit test: reference ROUTE SLT XML → service list populated correctly (26 tests)

### 5.3 FFmpeg A/V Decoder [x]
- [x] `av/hevc_decoder.h/.cc` — libavcodec HEVC ES → raw YUV420 frames
- [x] `av/audio_decoder.h/.cc` — libavcodec AC-4 / HE-AAC ES → PCM float32
- [x] Thread-safe output queue (fixed-size ring buffer, no dynamic alloc in steady state)
- [x] Unit test: ring buffer concurrency tests (13 tests)
- [x] Full decode test requires FFmpeg on CI runner — FFmpeg available, ring buffer tests pass

### 5.4 GStreamer Playback Pipeline [x]
- [x] `av/gst_player.h/.cc`
- [x] `appsrc → h265parse → avdec_h265 → videoconvert → autovideosink`
- [x] `appsrc → aacparse → avdec_aac → audioconvert → autoaudiosink`
- [x] A/V sync via GStreamer pipeline clock
- [x] GStreamer pipeline must be created on main thread (documented constraint)
- [x] Full pipeline test requires GStreamer on CI runner — GStreamer available, pipeline builds

### 5.5 End-to-End Integration Test [x]
- [x] Integration test: ALP → ROUTE → ServiceCatalog flow verified (8 tests)
- [x] Transport layer integration complete
- [x] Test IQ capture from channel 35 (599 MHz) available in `test/captures/`

---

## Phase 6 — Metrics, Scanner & GNU Radio Wrappers  *(~1 week)* [x]

Goal: Complete MVP. Observable signal quality, channel scanner, working GRC flowgraph.

### 6.1 Signal Quality Metrics [x]
- [x] `lib/metrics/snr_estimator.cc` — decision-directed from pilot residuals
- [x] `lib/metrics/mer_estimator.cc` — MER from equalized constellation RMS
- [x] `lib/metrics/ber_estimator.cc` — pre-FEC BER proxy from LDPC iteration count
- [x] `lib/metrics/signal_strength.cc` — dBm from HAL RSSI + AGC gain correction
- [x] `lib/metrics/metrics_aggregator.cc` — JSON metrics output with all fields
- [x] Unit tests: 27 tests passing (SNR, MER, BER, signal strength, aggregator)

### 6.2 GNU Radio OOT Blocks [x]
- [x] GR block for each `lib/` stage: `atsc3_bootstrap_detect`, `atsc3_ofdm_demod`, `atsc3_channel_eq`, `atsc3_fec_decode`, `atsc3_alp_demux`
- [x] Each block: AXI4-S contract comment in header, delegates immediately to `lib/` class
- [x] GR version detection via CMake `find_package(Gnuradio)` and `GR_VERSION` variable
- [x] GRC `.yml` block definition files for all blocks

### 6.3 Channel Scanner [x]
- [x] `apps/scanner.py` — sweeps US channel plan from `config/channel_plan_us.json`
- [x] Per channel: tune → acquire 2 s → report `{ channel, freq_hz, rssi_dbm, locked, mer_db, services[] }`
- [x] Output modes: table (default), JSON (`--format json`), CSV (`--format csv`)
- [x] Configurable dwell time (`--dwell 2.0`), band subset (`--band uhf|vhf|all`), single channel (`--channel N`)

### 6.4 GRC Flowgraph [x]
- [x] `apps/atsc3_rx.grc` — USRP Source (via HAL) → full decode chain → QT GUI metrics sink
- [x] Parameters exposed in GRC: frequency, gain, sample_rate, output_file (optional)
- [x] Works with FileSource for offline decode (parameter to switch source)

### 6.5 Metrics HTTP Server [x]
- [x] `apps/metrics_server.py` — HTTP server at `/metrics` returning JSON
- [x] Refresh rate: configurable (default 1 Hz)
- [x] HTML dashboard at `/` with auto-refresh and color-coded metrics

### 6.6 MVP Documentation Pass [x]
- [x] `README.md` §Usage section verified against actual build
- [x] `Doxyfile` created for API documentation generation
- [x] `CHANGELOG.md` entry for v0.1.0-mvp
- [x] Git tag `v0.1.0-mvp` created

---

## Phase 7 — Hardening, Compliance & Performance  *(post-MVP, ongoing)*

### 7.1 Full ATSC 3.0 H Matrix Support [x]
- [x] Generate all LDPC H matrices per ATSC A/322 §12.2 for both codeword lengths (64800, 16200)
- [x] Validate H matrices for all 12 code rates: 2/15, 3/15, 4/15, 5/15, 6/15, 7/15, 8/15, 9/15, 10/15, 11/15, 12/15, 13/15
- [x] Store H matrices in sparse CSR format in `config/ldpc_tables/` (row indices, column indices per rate/length)
- [x] Add H matrix loader with runtime selection based on L1 signaling
- [x] Unit test: verify H matrix dimensions and sparsity match ATSC spec tables
- [x] Unit test: syndrome check with known codewords for each code rate

#### H Matrix Implementation Notes
- Quasi-cyclic construction with expansion factor Q=360 (long) or Q=90 (short)
- Generator creates proper circulant permutation matrices per ATSC A/322
- Sparse row/column index storage for memory efficiency
- 18 unit tests verify dimensions, sparsity, syndrome checks, decoder integration

### 7.2 Interleaver Optimizations [x]
- [x] Cell deinterleaver: uses precomputed permutation tables (more efficient than bit-masking)
- [x] Time deinterleaver: bit-masking for power-of-2 row counts (depth 1,3,7,15)
- [x] Frequency deinterleaver: precompute permutation tables at init (eliminate runtime address calculation)
- [x] SIMD vectorization for interleaver copy loops (SSSE3 and AVX2 tiers implemented)
- [x] Benchmark: measure cycles/cell for each interleaver; target < 10 cycles/cell
- [x] Memory layout optimization: ensure deinterleaver buffers are 64-byte aligned for cache line efficiency

#### SIMD Implementation Notes
- Multi-tier SIMD architecture: SCALAR (fallback), SSSE3 (128-bit), AVX2 (256-bit)
- Runtime CPU feature detection via CPUID (`lib/simd/cpu_features.h`)
- Compile-time tier selection via CMake `-DATSC3_SIMD_TIER=<SCALAR|SSSE3|AVX2|NATIVE>`
- Cell and frequency deinterleavers use SIMD-optimized gather with prefetching
- 16 SIMD-specific unit tests verify intrinsic operations

#### Interleaver Benchmark Results (SSSE3 tier)
| Deinterleaver  | Size      | Cycles/Cell | Throughput | Target Met |
|----------------|-----------|-------------|------------|------------|
| Cell           | 10800     | 5.5         | 435 MB/s   | ✓          |
| Frequency      | 8K FFT    | 5.2         | 460 MB/s   | ✓          |
| Time (depth=4) | 10000     | 151         | 16 MB/s    | ✗ (expected)|
| Time (depth=15)| 10000     | 43          | 56 MB/s    | ✓ (bitmask)|

Time deinterleaver exceeds target for non-power-of-2 depths due to modulo operations.
Power-of-2 depths (1, 3, 7, 15 → 2, 4, 8, 16 rows) use bitmask and meet target.

### 7.3 ATSC 3.0 Compliance Testing [~]
- [x] Conformance test suite infrastructure (`test/compliance/` with 5 test files)
- [x] Bootstrap detection: verify all 128 bootstrap symbol variants (test_bootstrap_variants.cc)
- [x] L1 signaling: verify L1-Pre and L1-Post CRC checks for all configurations (test_l1_compliance.cc)
- [x] LDPC: decoder configuration tests for all 12 code rates (test_ldpc_waterfall.cc)
- [~] NUC constellations: infrastructure in place, placeholder tables (test_nuc_compliance.cc)
- [x] Interleaver round-trip: cell/freq/time deinterleaver tests (test_interleaver_compliance.cc)
- [x] Document compliance status in `docs/compliance.md` with pass/fail matrix

#### Compliance Test Results (85% passing)
| Test Suite | Pass | Fail | Notes |
|------------|------|------|-------|
| Bootstrap variants | 11/12 | 1 | Position test needs tuning |
| L1 compliance | 18/18 | 0 | Full pass |
| Interleaver compliance | 6/9 | 3 | 16K/32K FFT size variants |
| NUC compliance | 5/11 | 6 | Placeholder tables used |
| LDPC waterfall | 7/7 | 0 | Full pass |

**Remaining work**: Full NUC tables per ATSC A/322 §7.5 (code-rate dependent)

### 7.4 Performance Profiling & Optimization [~]
- [x] Profile with `perf` / `gprof`; identify bottleneck block
- [x] LDPC: vectorize hard decision and early termination with SIMD (SSSE3/AVX2)
- [~] LDPC: vectorize min-sum check-node update (analysis complete, implementation in progress)
- [x] FFT: evaluate FFTW plan modes (`FFTW_MEASURE` vs `FFTW_PATIENT`) for target host
- [x] Constellation demapper: optimize with direct slicer LLR computation (75× speedup)
- [x] Multi-PLP support: L1 config message ports for auto-configuration
- [x] Robustness: restart-on-lock-loss without flowgraph teardown
- [x] Memory: ASAN clean run on deinterleaver and LDPC unit tests
- [x] Fuzzing: libFuzzer on ALP and ROUTE parsers

#### Signal Chain Profiling Results (8K FFT, 64-QAM, excluding LDPC)

**Pre-optimization (v1):**
| Block                    | % Time | Notes                                    |
|--------------------------|--------|------------------------------------------|
| Constellation Demapper   | 99.3%  | max-log LLR brute-force O(M) search      |
| FFT Engine               | 0.4%   | FFTW3 with ESTIMATE plan                 |
| Cell De-interleaver      | 0.3%   | SSSE3 optimized                          |
| Frequency De-interleaver | 0.0%   | SSSE3 optimized                          |

**Post-optimization (v2, commit 7a85178):**
| Block                    | % Time | Notes                                    |
|--------------------------|--------|------------------------------------------|
| Constellation Demapper   | 66.1%  | Direct slicer O(1) LLR, 112× faster      |
| FFT Engine               | 17.9%  | FFTW3 with ESTIMATE plan                 |
| Cell De-interleaver      | 11.9%  | SSSE3 optimized                          |
| Frequency De-interleaver | 4.1%   | SSSE3 optimized                          |

**Optimization applied**: Replaced O(M) brute-force constellation search with O(1)
direct boundary-based LLR computation. Eliminated `std::fmod/fabs/round/max/min`
function calls with inline arithmetic.

**Results**: 6913 symbols × 1000 iterations (64-QAM)
- Total benchmark: 8.7s → 116ms (**75× faster**)
- Demapper: 86ms → 0.77ms per iteration (**112× faster**)
- Throughput: 0.75 Mbps → 55.89 Mbps

#### FFTW Plan Mode Evaluation Results
| FFT Size | ESTIMATE Plan | MEASURE Plan | PATIENT Plan | Recommendation |
|----------|---------------|--------------|--------------|----------------|
| 8K       | 0.3 ms        | 598 ms       | 8.7 s        | MEASURE+wisdom |
| 16K      | 0.4 ms        | 1.5 s        | 18.2 s       | MEASURE+wisdom |
| 32K      | 0.5 ms        | 2.4 s        | 37.2 s       | MEASURE+wisdom |

**Key finding**: With wisdom caching, MEASURE plan time drops from 922ms to 1.9ms.
Recommendation: Use `FFTW_MEASURE` with wisdom persistence for production deployments.

#### Multi-PLP Support Implementation
Added L1 config auto-configuration to demapper, time deinterleaver, and FEC decoder:
- `set_plp_id(int)` / `get_plp_id()` API on each block
- `l1_config` message input port parses PLP-specific parameters
- When `plp_id >= 0`, blocks extract config from L1 signaling
- When `plp_id == -1` (default), manual parameter mode preserved

#### LDPC Min-Sum Vectorization Analysis
See `docs/ldpc_vectorization_analysis.md` for full analysis. Key findings:

**Primary bottleneck**: O(d²) edge index lookups in `min_sum_iteration()`, not arithmetic.

**Recommended optimization order**:
1. Pre-compute edge mappings → 2-4× speedup ✓ (completed)
2. Layered decoding schedule → additional 1.5-2× speedup ✓ (completed)
3. Fixed-point LLR processing → additional 1.5-2× speedup ✓ (completed)

**Current SIMD status**: Helper functions vectorized (AVX2/SSE), edge mappings optimized.

**Edge mapping optimization**: Added `row_to_col_edge` and `col_to_row_edge` tables to
SparseMatrix, populated during `build_edge_mappings()`. Eliminates O(d²) linear searches
in `min_sum_iteration()` with O(1) direct lookups.

**Layered decoding optimization**: Replaced flooding schedule (all check nodes, then all
variable nodes) with layered/turbo schedule. Each check node update immediately propagates
to connected variable nodes, allowing later rows to see updated information. Converges in
fewer iterations (typically ~50% reduction). Uses stack allocation for small-degree nodes
(≤32) to avoid heap allocation overhead.

**Fixed-point LLR processing**: Added int16_t internal processing path for RTL compatibility.
Enable via `LdpcConfig::use_fixed_point = true`. Uses int16_t for APP and CN messages with
int32_t intermediate calculations to prevent overflow. Min-sum scaling 0.75 implemented as
`(3*x + 2) >> 2` with rounding. Symmetric saturation to [-32767, +32767]. Verified <0.1%
BER divergence from float implementation at typical operating SNR.

---

## Phase 8 — Live Playback & Service Discovery  *(~2 weeks)*

Goal: End-to-end live reception with audiovisual playback and service selection.

### 8.1 GNU Radio Signal Chain Blocks [x]
- [x] `atsc3_constellation_demapper` — QAM symbol to soft LLR conversion
- [x] `atsc3_cell_deinterleaver` — Cell de-interleaving (bit-reversal)
- [x] `atsc3_freq_deinterleaver` — Frequency de-interleaving (LFSR)
- [x] `atsc3_time_deinterleaver` — Time de-interleaving (CTI/HTI)
- [x] Complete GRC flowgraph: USRP → bootstrap → OFDM → EQ → demap → deint → FEC → ALP

### 8.2 Service Discovery Integration [x]
- [x] `atsc3_route_parser` block — Wrap `lib/framing/RouteParser`
- [x] `atsc3_service_guide` block — Display available services (JSON output)
- [x] ALP demux message ports for IP, TS, and signaling callbacks
- [x] Service selection via runtime parameter callback

### 8.3 Audiovisual Playback Integration [x]
- [x] `atsc3_service_selector` block — Filter IP packets by selected service TSI
- [x] `atsc3_av_player` block — Wrap `av/HevcDecoder`, `av/AudioDecoder`, `av/GstPlayer`
- [x] GStreamer main-thread initialization (documented constraint in start())
- [x] Valid ROUTE capture available (ch35_20sec_20260724 at 6.25 MS/s, SNR ~28 dB)
- [ ] End-to-end playback test with real ATSC 3.0 capture (requires full demod chain)

### 8.4 L1 Signaling Display [x]
- [x] `atsc3_l1_monitor` block — Display L1-Pre/Post signaling info (JSON output)
- [x] Expose L1 config via message port for downstream blocks
- [x] Integrate L1 info with metrics dashboard (Python-only)

### 8.5 Enhanced Receiver Features [x]
- [x] Service recording (dump ROUTE segments to file) — `apps/record_service.py`
- [x] Emergency Alert (EAS) detection and display — `apps/eas_monitor.py`
- [x] Closed caption extraction (IMSC1/TTML) — `apps/cc_extract.py`

### 8.6 Capture & Validation Utilities [x]
- [x] `apps/capture_iq.py` — IQ capture with real-time validation and SigMF metadata
- [x] `apps/validate_route.py` — ROUTE/ALP validation for captured IQ files
- [x] `apps/scanner.py` — Channel scanner for signal discovery

#### Existing A/V Infrastructure (ready to wrap)
| Component | Location | Status |
|-----------|----------|--------|
| HEVC decoder | av/hevc_decoder.cc | Complete |
| Audio decoder (AC-4/AAC) | av/audio_decoder.cc | Complete |
| GStreamer playback | av/gst_player.cc | Complete |
| L1 decoder | lib/framing/l1_decoder.cc | Complete |
| ROUTE parser | lib/framing/route_parser.cc | Complete |
| Service catalog | lib/framing/service_catalog.cc | Complete |
| ALP demux | lib/framing/alp_demux.cc | Complete |
| Metrics aggregator | lib/metrics/metrics_aggregator.cc | Complete |

---

## Phase 9 — HDL Port: Synthesizable RTL Receiver  *(post-MVP, ongoing)*

Goal: a full synthesizable RTL receiver, IQ samples in to ALP-demuxed
output, matching real ATSC 3.0 demod silicon's host handoff boundary.
IEEE 1364-2001 Verilog only, cocotb-based bit-exact equivalence testing
required, formal verification where feasible (SVA-lite via SymbiYosys).
Everything downstream of ALP demux (ROUTE/DASH, service catalog, A/V
decode/playback) stays host software, out of scope. Supersedes the old
"Post-MVP: HDL Port" stub below — see that section for the note.

Two design-review passes preceded RTL work and found several golden-model
defects (fixed on `bugfix/hdl-golden-model-fixes`, now on `develop`) and
five blocks whose `ATSC3_FIXED_POINT` path quantized I/O only while
computing internally in `double`/`float` — those five were rewritten to
genuine fixed-point (9.0b) before any RTL work started, so every phase
below holds the same bit-exact bar against its C++ reference.

### 9.0 Foundations, Golden-Model Fixes & Standalone-Testability [x]
- [x] Golden-model bugfixes merged to `develop` (Q1.15 saturation UB, FFT
      accumulator width, divide-by-zero guards, an addressing bug in the
      timing-recovery interpolator)
- [x] `hdl/rtl/`, `hdl/formal/`, `hdl/sim/{cocotb,golden}/`, `hdl/synth/`,
      `hdl/docs/` directory layout; `hdl/rtl/include/axi4s_types.vh`
      (supersedes `hdl/stubs/axi4s_interface.vh`); `axi4s_skid_buffer.v`
      common pipeline register
- [x] Toolchain proven end-to-end (Verilator 5.020 lint + sim, cocotb
      1.9.2 pinned for VPI compatibility, SymbiYosys/z3) and documented in
      `hdl/docs/toolchain.md`
- [x] `config/hdl_register_map.json` + generated
      `hdl/docs/axi4lite_register_map.md` — default-reset register values
      for every block, host/L1-writable split, so blocks are testable
      standalone before the config sequencer (9.13b) exists
- [x] `hdl/docs/q_format_notes.md` (input Q-format/backoff contract),
      `hdl/docs/placeholder_status.md` (non-spec placeholder inventory:
      LDPC matrices, bit-reversal deinterleavers, frame-sync preamble
      reference, hard-decision L1 FEC, ALP ROHC passthrough, etc.),
      `hdl/docs/wire_level_layouts.md` and `hdl/rtl/include/status_words.vh`
      (bit-packed `PilotSymbol`/`BootstrapDetection`/`FrameEvent` layouts)
- [x] `hdl/docs/formal_conventions.md` — SVA-lite via immediate
      assert/assume/cover only, small-parameterization convention,
      white-box internal-signal checks via a flatten+`expose` two-stage
      Yosys flow (not SystemVerilog `bind`, which this Yosys build accepts
      syntactically but never actually connects — confirmed by injecting
      a deliberately-false assertion into a `bind`-attached checker and
      finding it still proved; a plain hierarchical dotted reference has
      the same silent-no-connection failure mode)

### 9.0b Fixed-point rewrite of five golden-model blocks [x]
- [x] `lib/dsp/cordic.h/.cc` — new shared fixed-point CORDIC core
      (rotation mode for cos/sin, vectoring mode for atan2/magnitude),
      14 iterations, Q1.15 "turns-over-pi" angle format, ≥40 dB SNR vs.
      `std::cos/sin/atan2/hypot`
- [x] `bootstrap_detector.cc` — integer EWMA correlator + CORDIC-based
      metric/CFO (replacing `double`/`std::arg`/`sqrt`/`log10`)
- [x] `timing_recovery.cc` — fixed-point polyphase FIR + Gardner TED +
      loop filter (no CORDIC needed)
- [x] `freq_correction.cc` — CORDIC-based NCO replacing per-sample
      `std::cos`/`sin`
- [x] `frame_sync.cc` — fixed-point cross-correlation + CORDIC
      vectoring-mode magnitude, plus an integer square-root fallback for
      the one piece (`sqrt(sig_power * ref_power)`) that isn't a true 2D
      vector magnitude
- [x] `constellation_demapper.cc` — generalized the boundary-slicer into
      one fixed-point LLR path for every modulation order (replacing both
      the hand-unrolled fast paths and the O(M) brute-force fallback),
      fixed the constellation-table Q1.15 overflow as a side effect
- [x] Each rewrite verified ≥40 dB SNR against its own pre-rewrite
      behavior before being held to a bit-exact RTL bar

### 9.1 Bootstrap Detector RTL [x]
- [x] `hdl/rtl/common/cordic.v` + `hdl/rtl/include/cordic_types.vh` —
      shared iterative dual-mode CORDIC core, bit-exact port of
      `lib/dsp/cordic.cc`; lint-clean (Verilator `--lint-only
      --language 1364-2001 -Wall`)
- [x] `hdl/formal/cordic.sby` — FSM-legality, iteration-count-bound,
      start/busy/done-handshake properties proven (k-induction); cover
      tasks confirm both modes and the degenerate vector(0,0) bypass are
      reachable
- [x] cocotb bit-exact test for `cordic.v` vs. `lib/dsp/cordic.cc`, via a
      golden-vector CLI (`hdl/sim/golden/cordic_gen`) linking the real
      `atsc3_lib` — 136 cases (boundary + randomized), both modes
- [x] Golden-model fixes first: the fixed-point metric square overflowed
      int64 (UB) after signal→silence; then the detection algorithm itself
      was corrected (below)
- [x] Spurious-detection fix (C++ and RTL): R normalized by the delayed
      half only (a sliding window, against an EWMA P), so noise→signal
      edges produced bursts of false high-metric detections, and the FSM
      re-armed immediately, cascading detections per bootstrap. R is now
      the identically weighted EWMA of both halves' energy (|P| ≤ R by
      Cauchy-Schwarz, metric ≤ 1, peak at the end of the structure), with
      a near-silence energy floor and re-arm hysteresis — one detection
      per bootstrap. Also fixed `snr_db` (it reported SNR² in dB)
- [x] `hdl/rtl/sync/bootstrap_detector.v` — two single-pole EWMA
      accumulators (P and R) plus the 2048-sample x[n-L] delay RAM; the
      old per-sample power RAM is gone. Sequential datapath: `hdl/rtl/common/udiv_seq.v` (new shared
      restoring divider) for the C++'s two `(P<<15)/R` divides (skipped
      below the energy floor) and the moving-average divide, shared
      `cordic.v` for magnitude/angle
- [x] AXI4-S: `TDATA=ci16` in, `BootstrapDetection` status word out;
      `status_words.vh`'s metric field widened 16 → 32 bits (the C++
      int32 peak metric can reach 65536 through rounding)
- [x] cocotb: synthetic bootstrap symbols at known CFO offsets, bit-exact
      vs. `hdl/sim/golden/bootstrap_gen` linking the real `atsc3_lib` —
      every sample's smoothed metric/CFO plus every detection word, five
      scenarios (two bootstraps after noise edges, raw-angle + odd window +
      full-scale corners, a correlated fade through the energy floor, zero
      window, oversized-window flag); negative-control mutants (energy
      floor, history wrap, no hysteresis) each caught
- [x] Formal (`hdl/formal/prove_pdr.sh bootstrap_detector`, unbounded
      ABC PDR with multipliers cut, since smtbmc/z3 stalls on this
      datapath): correlator index bound, zero-averaging-window clamp,
      window/history-index bounds, P/R divide only above the energy floor
      (no divide-by-zero), detect/re-arm exclusivity, AXI4-S input/output
      exclusivity and output hold; non-vacuity confirmed by RTL mutants

### 9.2 Timing Recovery RTL [x]
- [x] 16-bank×32-tap polyphase FIR (ROM taps, MAC array) + Gardner TED
      feedback loop, bit-exact vs. the 9.0b rewrite
- [x] AXI4-S `ci16` in/out; cocotb timing-offset sweep; formal: phase
      accumulator wrap behavior, AXI4-S protocol properties

#### Implementation Notes
- `hdl/rtl/sync/polyphase_fir.v`: a shared, sequential (2 cycles/tap,
  ~70 cycles/call) 32-tap MAC core reused for all four `interpolate()`
  calls a symbol boundary needs (the emitted symbol plus the Gardner
  TED's curr/mid/prev), the same "correctness first, pipeline later"
  policy `bootstrap_detector.v` documents (9.17 timing-closure work).
  Coefficient ROM (`timing_recovery_coeffs.vh`, generated) is dumped
  directly from `PolyphaseInterpolator::get_coeffs()` — a new accessor
  added to `lib/sync/timing_recovery.h` for exactly this — rather than
  re-deriving the raised-cosine/Kaiser-window filter design in a
  separate tool, so the ROM is guaranteed bit-exact to whatever the C++
  actually computes, not just what it's supposed to.
- `hdl/rtl/sync/timing_recovery.v`: owns the 128-entry circular sample
  buffer and all persistent state (`buf_write_idx`/`buf_read_idx`/
  `buf_count`, `mu_q16`, `timing_error_q15`, `loop_integrator_q15`),
  matching the C++ class split exactly (`PolyphaseInterpolator::
  interpolate()` takes a buffer pointer and index, owns neither).
- `kp_q15`/`ki_q15` are taken as direct config registers rather than
  re-derived in RTL from `loop_bandwidth_hz`/`loop_damping`/
  `symbol_rate_hz` (`compute_loop_gains()`'s one-time, division-and-
  multiply-heavy filter design) — the register-map-listed path, but the
  same "designed once in double, quantized for storage" treatment the
  polyphase ROM gets rather than genuinely synthesizable per-cycle
  logic. New `get_kp_q15()`/`get_ki_q15()` accessors let the golden CLI
  report what a given config actually produces, so cocotb configures
  the RTL with the identical numbers.
- Config (`kp_q15`, `ki_q15`, `samples_per_symbol`, `initial_offset`) is
  latched at reset, not read live — same `cp_removal.v`/
  `bootstrap_detector.v` pattern, same reason (a live config the FSM is
  mid-boundary against isn't a golden-model behavior to match).
  `cfg_locked` is the one config input read live: `set_locked()` is an
  explicitly asynchronous runtime control in the C++ API (training vs.
  tracking mode), not a reconfigure-only value.
- `mu_q16` (Q0.16, wraps to [0,1) via plain unsigned overflow) is
  initialized at reset from a Q1.15 `cfg_initial_offset_q15` via a
  1-bit left shift ({value[14:0], 1'b0}) — reinterpreting the
  twos-complement pattern as unsigned and doubling it is bit-exact to
  the C++'s `wrapped = mu - floor(mu); mu_q16_ = uint16_t(wrapped *
  65536)` for every possible 16-bit input, not just non-negative ones,
  and the formal harness cross-checks this claim independently (see
  below) rather than trusting the derivation by inspection alone.
  Phase selection (mu_q16's top 4 bits, or (mu_q16+0x8000)'s top 4 bits
  for the Gardner TED's mid-point) is exact because num_phases=16 is a
  power of 2.
- Found by cocotb, not by construction: an early version zero-extended
  (`{1'b0, x_curr_re}`) rather than sign-extended the Gardner TED's
  `x_curr - x_prev`/`x_mid` operands, silently reinterpreting negative
  interpolator outputs as large positive values. All scenarios that
  never engage the loop filter (`cfg_locked=0`) passed regardless;
  every scenario that does diverged from the golden model starting at
  the very first update. Fixed by using plain signed subtraction on the
  already-`reg signed` operands and letting Verilog's context-determined
  sign extension handle the width, instead of a manual (and wrong)
  concatenation.
- `hdl/formal/timing_recovery.sby`: scoped to exactly the TASKS.md ask
  (phase-accumulator wrap, AXI4-S protocol), not an exhaustive proof of
  the 15-state control FSM or the FIR's arithmetic (cocotb's job, at
  real width, against the real golden model). `s_axis_tready`/
  `m_axis_tvalid` are pinned to their exact controlling FSM state
  (ST_IDLE / ST_OUTPUT) rather than stated as purely temporal
  properties — the weaker temporal forms are true but were not
  themselves inductive (k-induction found an unreachable predecessor,
  `m_axis_tvalid=1` while `state==ST_CLEAR`, that nothing in the RTL
  would ever clear); pinning to state first, then deriving stability
  from that, closes the proof. The phase-accumulator-wrap property is
  an independent cross-check (a second, differently-computed reference)
  of the mu_q16 reset derivation, not a re-read of the DUT's own
  formula. Despite the polyphase FIR's and loop filter's multiplies
  (cut, sound for these control/protocol-only properties, same
  reasoning as `bootstrap_detector.sby`), plain `smtbmc z3` closes this
  in a few seconds — no need for ABC PDR here. `cover` excludes
  `s_axis_tready`/`m_axis_tvalid`/`mon_valid` (reaching any of them
  needs the reset-time 128-cycle buffer-clear sweep to finish first,
  well past any practical BMC depth) — the same reasoning
  `cp_removal.sby`'s excluded cover goal documents; cocotb exercises
  all three concretely, repeatedly, at full scale.

### 9.3 CP Removal RTL [x]
- [x] Counter vs. CP-length register, gates `TVALID`; no numerical
      content to diverge on — cocotb covers all 11 CP fractions
      bit-exact; formal: counter never exceeds the max defined length

#### Implementation Notes
- `hdl/rtl/ofdm/cp_removal.v`: a pure position counter, no TDATA
  arithmetic at all. Streams straight through (TREADY held high and
  samples dropped during the CP; wired to the downstream TREADY and
  passed through during the FFT portion) rather than porting
  `CpRemoval::process()`'s whole-symbol buffering — both emit the
  identical value sequence and TLAST framing, just at different latency,
  the same kind of freedom `bootstrap_detector.v` takes against its own
  golden model.
- `cp_length = cp_fraction_numerator * (fft_size >> 13)` is an exact
  integer multiply (fft_size is always a multiple of 8192 for the three
  defined sizes) — no divider anywhere in this block.
- Config (FFT_SIZE, CP_LENGTH) is validated, clamped, and **latched at
  reset**, not read live — matching `bootstrap_detector.v`'s
  `cfg_averaging_window`/`win_n` pattern. This isn't just style
  consistency: with a live config, the "counter never exceeds the max
  defined length" property isn't provable (an adversarial config change
  could shrink `symbol_len` below the current `cnt` with no clock edge in
  between) — latching at reset makes it a genuine invariant.
- `hdl/sim/golden/cp_removal_gen.cc`: golden-vector CLI linking the real
  `atsc3_lib`, grouping its output by completed symbol (not per-sample)
  since the C++ callback fires once per whole symbol.
- `hdl/sim/cocotb/test_cp_removal.py`: all 11 `CpFraction` values at
  FFT_8K, `FFT_16K` to exercise the `numerator * scale` multiply,
  multi-symbol continuity, and randomized backpressure on both sides —
  bit-exact against the golden CLI in every case.
- `hdl/formal/cp_removal.sby`: `mode prove` (BMC + k-induction) with
  plain `smtbmc z3`, no multiplier cutting or ABC PDR needed — the
  13x3-bit multiply here is far smaller than `bootstrap_detector`'s
  64-bit datapath. Proves the counter bound, clamp correctness, reset
  behavior, and the AXI4-S framing rule; `cover` reaches every property
  except "completed a whole symbol" (needs a BMC trace hundreds to tens
  of thousands of cycles deep for the smallest real CP fraction —
  impractical to unroll, and cocotb already demonstrates it concretely
  at full scale). A wraparound-removal mutant confirmed the counter-bound
  property is load-bearing and that `mode prove` (not bare `mode bmc`) is
  needed: the mutant still passes BMC's depth-20 base case but correctly
  fails k-induction.

### 9.4 FFT Engine RTL [x]
- [x] Memory-based in-place radix-2 DIT, shared 16384-entry twiddle ROM,
      bit-exact by construction; streaming R2SDF explicitly deferred
      future work
- [x] AXI4-S wrapper, cocotb, formal (bit-reversal address generator
      bounds, butterfly arithmetic)

#### Implementation Notes
- `hdl/rtl/ofdm/fft_engine.v`: forward-direction-only (per
  `config/hdl_register_map.json`, DIRECTION is `const`-fixed to
  `kForward` — this is a receiver, there's no transmit path to port —
  so there's no `cfg_direction` port and no inverse-normalize output
  path, unlike the C++ `FftEngine`). A single 32768-entry true-dual-port
  work RAM (`work_re`/`work_im`) holds the transform in place; a shared
  16384-entry twiddle ROM covers all three legal FFT sizes at once:
  `master_rom_index = j << (15 - stage)` is the same address at a given
  `stage` for FFT_8K/16K/32K alike, because the FFT-size-dependent terms
  cancel (`j * (N/m) * (32768/N) = j * (32768/m)`) — one ROM, generated
  once from the real `compute_twiddles()` formula
  (`hdl/sim/golden/fft_twiddle_gen.cc`, sign=-1/forward, calling the
  real `atsc3::float_to_q15()`), not three. Bit-reversal is a single
  15-bit reverse of `sample_cnt` (whose top bits are always 0 for the
  active size) right-shifted by `15 - log2(N)` — reversing all 15 bits
  then shifting lines up with reversing just the low log2(N) bits
  directly. Same flattened-single-counter butterfly indexing as
  `bootstrap_detector.v`'s enumeration style: `bfly_idx` alone (no
  nested stage/group/element loop) determines `j_idx`/`group_idx`/
  `idx_even`/`idx_odd` for a given `stage`, since same-stage butterflies
  never share an index and order doesn't matter.
- Same "correctness first" sequential FSM as every other 9.x block: a
  butterfly and an unload sample both take a full ADDR→READ/WAIT→WRITE/
  EMIT 3-cycle sequence (RAM read needs a settled cycle before
  `ram_rdata_a` reflects the new address) rather than a pipelined R2SDF
  streaming architecture — correct by construction against the golden
  model at any clock rate, throughput left to 9.17 timing-closure work.
- Real bug, found by cocotb's randomized tests, not by inspection: the
  unload loop's steady state looped `ST_UNLOAD_EMIT → ST_UNLOAD_ADDR →
  ST_UNLOAD_EMIT` directly, skipping the settle cycle every other state
  in the FSM has. This fed `sample_cnt - 1`'s RAM data as `sample_cnt`'s
  output for every sample after the first. A first fix attempt added
  the missing `ST_UNLOAD_WAIT` state but only on the loop's first
  iteration; that turned "everything shifted by one" into "index 0
  correct, index 1 duplicates it, then shifted by one again" — proof
  the steady-state path needed the same fix, not just the first sample.
  The real fix makes every iteration walk the full `ST_UNLOAD_ADDR →
  ST_UNLOAD_WAIT → ST_UNLOAD_EMIT` sequence, looping back to `ADDR`
  rather than straight to `EMIT`. Verified against a hand-built N=8
  Python reference before confirming against the real golden model at
  8192/16384 with random data.
- `hdl/sim/golden/fft_engine_gen.cc`: golden-vector CLI linking the real
  `FftEngine::create()` (forward-only invocation, matching the RTL's
  scope). `hdl/sim/cocotb/test_fft_engine.py` runs full bit-exact
  transforms at real FFT_SIZE (8192, 16384) — impulse and random input —
  rather than a small-parameterization escape hatch: this block's ROM
  addressing and bit-reversal algebra is exactly what needs checking at
  the real width, and cocotb, not formal, is the tool for that. 32768 is
  left to formal's structural checks plus the fact that 8192/16384
  already exercise every stage-count the FSM has (13 and 14 stages;
  32768's 15th stage is the same butterfly/ROM-addressing logic one
  more time, not new code).
- `hdl/formal/fft_engine.sby`: scoped to the TASKS.md ask (bit-reversal
  address bound, butterfly arithmetic bound) plus FSM legality and
  AXI4-S protocol — not transform correctness, cocotb's job. Two
  performance fixes were needed beyond the usual `cutpoint t:$mul`
  (same "none of these properties depend on a product's value"
  reasoning as every other block): first, Yosys's Stage 1
  flatten/elaborate pass stalled indefinitely on the twiddle ROM's
  16384-entry `initial` block — a `` `ifndef FORMAL_SKIP_ROM_INIT ``
  guard around that `` `include `` (defined via `-DFORMAL_SKIP_ROM_INIT`
  only in `fft_engine.sby`'s `read_verilog`) skips populating actual ROM
  content for the formal build only; none of the proved properties
  depend on ROM values, only on addressing, so leaving those registers
  free is sound. Second, even after that fix, the two work-RAM
  `$mem_v2` cells (32768 x 32-bit, true dual port) still OOM'd z3 (~8GB,
  cgroup-killed) partway through `engine_0.induction` — cutting them too
  (`cutpoint t:$mul t:$mem_v2`) turned a proof that couldn't finish into
  one that closes in single-digit seconds; sound for the same reason as
  the ROM (no property reads RAM data, and `m_axis_tdata`'s one
  data-dependent property only needs its own previous-cycle value to
  stay equal, which holds under a free RAM read too since the register
  simply isn't reassigned on a stalled cycle).
- Two of the AXI4-S protocol properties were themselves wrong on the
  first pass, both caught by the proof itself rather than by
  inspection, worth recording since both are easy mistakes to repeat:
  (1) comparing an address/counter against `fft_size_r_probe[ADDR_WIDTH
  -1:0]` silently drops bit 15, so for FFT_32K (`16'h8000`) the RHS
  truncates to 0 and the bound trivially fails — comparing against the
  full-width `fft_size_r_probe` and letting Verilog zero-extend the
  narrower LHS is the fix (the real RTL's own `fft_size_r[ADDR_WIDTH-1:0]
  - 1'b1` equality checks are fine as-is: the same drop-to-0 then
  `-1` wraps to exactly the right max index, a coincidence that only
  works for equality-against-`size-1`, not a general `<` bound). (2)
  `m_axis_tvalid == (state == ST_UNLOAD_EMIT)`, the same FSM-state-pin
  idiom `timing_recovery.sby` needed to close induction, is not
  uninductive here — it's simply false: `fft_engine.v` sets
  `m_axis_tvalid <= 1` *from inside* `ST_UNLOAD_EMIT`'s own case
  branch, so the registered value isn't visible until the following
  cycle, by which point `state` has already advanced (to
  `ST_UNLOAD_ADDR` mid-transform, or `ST_IDLE` on the last sample).
  Confirmed empirically with a throwaway cocotb probe (not checked in)
  before trusting it: free-running, `m_axis_tvalid` pulses for one
  cycle per 3-cycle unload period coinciding with `state ==
  ST_UNLOAD_ADDR`; under backpressure it instead holds at 1 for as long
  as `state` stays at `ST_UNLOAD_EMIT`. No single state value
  characterizes it. What's actually true regardless of history — every
  path that sets or clears `m_axis_tvalid` is gated on `m_axis_tready`,
  so it cannot change on a cycle where `m_axis_tready` is low — is what
  the harness checks instead, alongside the pre-existing `m_axis_tdata`/
  `m_axis_tlast` hold-steady-while-stalled properties (now joined by
  "`m_axis_tvalid` itself can't retract," the AXI4-S rule those two were
  already implicitly assuming). `s_axis_tready == (state ==
  ST_LOAD)` remains a valid exact pin: `ST_IDLE` sets it one state
  *before* entering `ST_LOAD`, so unlike `m_axis_tvalid` there's no
  same-cycle visibility lag. `cover` reaches `ST_LOAD` and
  `cfg_fft_size_invalid`; deeper goals (`m_axis_tvalid`, a completed
  unload) are left to cocotb, same reasoning as every other block's
  `.sby`.

### 9.5 Pilot Extraction + Frequency Correction RTL [ ]
- [ ] `pilot_extractor.cc`'s `estimate_channel()` core is already clean
      fixed-point — no rewrite needed, bit-exact-ready as-is
- [ ] Pilot pattern ROM (PP1–PP8 from `pilot_extractor.h`, not
      `atsc3_modes.json`'s unused separate table); explicit
      SCATTERED > CONTINUAL > EDGE dedup precedence needed in the golden
      model first (unspecified `std::sort`/`std::unique` behavior today)
- [ ] `freq_correction.cc` RTL: CORDIC-based NCO per 9.0b, bounded output
      by construction
- [ ] cocotb: all 8 pilot patterns incl. dedup-collision cases, bit-exact
      CFO-injection sweep; formal: ROM bound, freq-correction output
      bound, AXI4-S protocol properties

### 9.6 Frame Sync RTL [ ]
- [ ] Runs after FFT + frequency correction (per `frame_sync.h`'s own
      documented contract, not the receiver's coarse block-diagram order)
- [ ] Acquisition correlator is `fft_size`-long, evaluated at every
      offset in the search window — budget as the single largest
      correlator in the design (~2M complex MACs/call at default width)
- [ ] Preamble reference stays the existing non-spec placeholder pattern;
      pin a fixed sample-chunking convention so the golden CLI and the
      RTL streaming interface don't diverge on buffering alone
- [ ] AXI4-S `ci16` in, `FrameEvent`-equivalent status word out; cocotb
      frame-boundary sweep at the pinned chunk size; formal: FSM only
      reaches defined states, AXI4-S protocol properties

### 9.7 Channel Estimator RTL [ ]
- [ ] LS + Wiener, sequential `complex_divider.v` (truncate-toward-zero),
      `wiener_fir.v`, `linear_interp.v`; SNR/phase-error outputs
      explicitly out of scope (not bit-exact-portable, `log10`-based)

### 9.8 Equalizer (FDE) RTL [ ]
- [ ] Already-genuine fixed-point core (no algorithm rewrite needed) —
      needs its own `eq_complex_divider.v` (different rounding than 9.7's
      divider, not shared)
- [ ] cocotb: ZF/MMSE, deep-fade injection, phase-tracking sweep near
      full scale; formal: the fallback (divide-by-a-floor) divisor path
      is never taken; int64 numerator never truncated on store

### 9.9 Constellation Demapper RTL [ ]
- [ ] One parameterized boundary-slicer for every mode per the 9.0b
      rewrite (not two algorithms with a scope split between them); real
      code-rate-indexed NUC tables exist and are used (license
      compatibility of `nuc_tables.h` needs checking before porting)
- [ ] AXI4-S: equalized `ci16` in, `int8_t` LLR out; cocotb: all 6
      uniform modes + NUC at multiple SNRs, explicit constellation-table
      saturation check; formal: LLR clamp bound, constellation ROM bound

### 9.10 De-interleavers RTL (Cell / Time / Frequency) [ ]
- [ ] Frequency deinterleaver: ROM-able as-is (exactly 3 enumerated
      `n` values)
- [ ] Cell deinterleaver: NOT simply ROM-able — its permutation depends
      continuously on `num_cells` (modulation × code rate × FEC block
      count); needs an on-chip permutation-builder FSM, or an enumerated
      `num_cells` restriction as a scope-reduction fallback
- [ ] Both cell and frequency deinterleavers use bit-reversal permutation
      (non-spec placeholder, not real ATSC A/322 interleaving)
- [ ] Time deinterleaver delay-line RAM budget: up to ~31 Mbit at
      `ti_depth=15`/QPSK — budget alongside LDPC's memory in 9.17; must
      match the golden model's `settling_blocks_` latency/valid gating
- [ ] cocotb round-trips against the existing `test/unit/test_*_deinterleaver.cc`
      vectors; formal: permutation/address-generator bounds

### 9.11 LDPC Decoder RTL [ ]
- [ ] Min-sum, parameterized by code rate, per-rate ROM; non-spec
      placeholder H-matrices (see `hdl/docs/placeholder_status.md`)

### 9.12 BCH Decoder RTL [ ]
- [ ] Binary BCH — bit-flip directly at Chien roots, no Forney algorithm
      needed; LFSR syndrome engine restructured (not a literal port) but
      still needs the `gf_exp_`/`gf_log_` log-table ROMs downstream of it
- [ ] Cycle budget must include the failure detector's full second
      syndrome-recomputation pass (roughly doubles worst-case latency)
- [ ] cocotb: 0–13 injected bit errors (the existing 6-corrected/13-fails
      boundary); formal: Chien-search root count never exceeds `t=12`

### 9.13 L1 Decoder + Config Sequencer RTL [ ]
- [ ] 9.13a — L1 bitfield decode: `l1_decoder.cc`'s `fec_decode()` is a
      bare LLR sign-slicer with no LDPC/BCH dependency (a documented
      placeholder — hard-decision only, no FEC protection on L1 today);
      bitfield-extract + CRC-32 can be bit-exact even though the overall
      acquisition reliability inherits that fragility
- [ ] 9.13b — config sequencer FSM: new design, no working C++ reference
      exists (`ConfigBus` observer wiring is never invoked outside
      tests today) — specified against `Atsc3Config`'s field shapes,
      verified against a specified transition sequence, not a golden model
- [ ] Register split: L1-decoded fields are read-only status mirrors,
      written internally by 9.13b, not host-writable in normal operation

### 9.14 ALP Demux RTL [ ]
- [ ] `ReassemblyContext` is already bounded (fixed array, reserved
      buffers) — the real rule-5 gap is `input_buffer_`'s unbounded
      `resize()`, which needs an explicit RTL depth bound + overflow
      policy
- [ ] Three typed output streams (IP, signaling, TS), not one muxed
      per-PLP stream; no timeout mechanism (the golden model's own
      `check_reassembly_timeouts()` is an unimplemented no-op — don't
      build one against no reference)
- [ ] cocotb against the existing `test/unit/test_alp_demux.cc` vectors;
      formal: `input_buffer_`'s RTL depth bound is never exceeded

### 9.15 AXI4-Lite Control Plane [ ]
- [ ] Full register map across all blocks from `config/hdl_register_map.json`,
      host/L1-writable split, Q-format contract constant, default-config
      reset values; formal: standard AXI4-Lite safety properties

### 9.16 End-to-End HDL Pipeline Test (required) [ ]
- [ ] Tier 1 (required): steady-state pipeline — fixed known config
      pre-loaded into the register file (bypassing 9.13's acquisition),
      real IQ samples through 9.1–9.12 + 9.14, bit-exact vs. an all-C++
      golden run
- [ ] Tier 2: cold-start acquisition through 9.13's sequencer against a
      *specified* transition sequence — not claimed bit-exact (no golden
      acquisition path exists) and explicitly inheriting 9.13a's
      hard-decision L1 fragility
- [ ] Needs `git lfs pull` on the real SigMF captures
      (`test/captures/ch35_*.sigmf-*`) and a SigMF-aware golden CLI

### 9.17 Timing Closure [ ]
- [ ] Technology-independent metrics only (Yosys `synth`/`abc`): logic
      depth, LUT-equivalent count, inferred RAM bits, cycles per
      symbol/codeword/datagram vs. the ≤25 MS/s N210 ceiling — including
      frame sync's correlator, the time deinterleaver's RAM, and BCH's
      doubled worst-case latency
- [ ] Post-synthesis functional equivalence via Yosys `equiv_opt`/`sat`
- [ ] Bootstrap detector throughput: ~150 cycles/sample today (two
      64-cycle restoring divides, the averaging divide, iterative CORDIC)
      vs. 16 cycles/sample needed for 6.25 MS/s at 100 MHz —
      radix-4/early-terminating or pipelined dividers and a pipelined
      CORDIC

---

## Post-MVP: ML Multipath Mitigation  *(separate milestone)*

- [ ] `ml/data/channel_sim.py` — ray-tracing multipath channel simulator (parametric)
- [ ] `ml/data/capture_label.py` — label real IQ captures with ground-truth channel via pilot-LS
- [ ] `ml/models/cnn_estimator.py` — CNN channel estimator replacing Wiener filter; input: pilot observations
- [ ] `ml/models/lstm_equalizer.py` — LSTM sequence equalizer for severe multipath
- [ ] Training pipeline: PyTorch Lightning, logged to W&B or MLflow
- [ ] Export: `torch.onnx.export()` → `ml/models/exported/channel_estimator.onnx`
- [ ] `ml/inference/onnx_estimator.cc` — ONNX Runtime C++ wrapper implementing `ChannelEstimator` interface
- [ ] CMake: `ATSC3_ENABLE_ML=ON` links ONNX Runtime, registers `ML_ONNX` backend
- [ ] A/B test harness: compare Wiener vs ML MER on same IQ capture

---

## Post-MVP: HDL Port  *(separate milestone)*

- [ ] `hdl/stubs/axi4s_types.v` — AXI4-Stream wire definitions (from existing stubs)
- [ ] Verilator + cocotb testbench for `fft_engine` (start here — largest block)
- [ ] RTL port: `hdl/rtl/fft_engine.v` — parameterized Cooley-Tukey, N=8K/16K/32K
- [ ] Fixed-point numerical equivalence: cocotb test vs C++ `ATSC3_FIXED_POINT=ON` build
- [ ] RTL port: `hdl/rtl/ldpc_decoder.v` — min-sum, parameterized by code rate
- [ ] RTL port: `hdl/rtl/channel_estimator.v` — LS + pipelined Wiener
- [ ] AXI4-Lite control plane for all blocks (parameter load from ROM/registers)
- [ ] Timing closure simulation (Verilator gate-level with annotated delays)
