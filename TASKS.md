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
- [ ] CMake dependency graph target (`cmake --graphviz`); verify no upward-layer imports
- [x] `clang-format` config (`.clang-format`, based on Google style, 100-col)
- [x] `cppcheck` suppressions file (`.cppcheck`)
- [x] `pre-commit` config (clang-format, trailing whitespace, no large files without LFS)

### 0.2 CI/CD Pipelines
- [x] `.github/workflows/ci.yml` — Ubuntu 22.04, GR 3.10, UHD 3.15, build + `ctest -L unit`
- [x] `.github/workflows/nightly.yml` — GR 3.10 build, long IQ tests, lcov coverage, Doxygen deploy
- [x] `.github/workflows/release.yml` — tag-triggered, cpack `.deb`, GitHub Release
- [x] `docker/Dockerfile.ci` — pinned apt packages, pushed to `ghcr.io` on nightly
- [x] `docker/versions.lock` — records exact apt package versions for reproducibility
- [ ] Branch protection: require `ci.yml` green; no force-push to `main`

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
- [~] CI: `ctest -L unit` runs and reports; badge added to `README.md`

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
- [x] `set_gain()` — maps to TVRX gain range (0–95 dB); clamped with warning
- [x] `set_sample_rate()` — validates N210 sustainable rates over GigE (≤25 MS/s)
- [x] `get_rssi_dbm()` — reads UHD sensor `"rssi"` from TVRX daughterboard
- [ ] Hardware test (manual, `ctest -L hw`): loopback noise floor, RSSI reads plausible value

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
- [ ] Fixed-point mode: verify equivalence within 40 dB SNR threshold

### 2.2 Frame Timing & Sync [x]
- [x] `lib/sync/timing_recovery.h/.cc` — Gardner TED + polyphase interpolator (32-tap, 16-phase)
- [x] `lib/sync/frame_sync.h/.cc` — superframe and subframe boundary tracker using preamble correlation
- [x] Unit test: timing recovery and frame sync tests (30 tests passing)
- [ ] Integration test: FileSource with known offset IQ → frame_sync locks within 10 frames

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
- [ ] Fixed-point equivalence test: float vs fixed SNR ≥ 40 dB

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

## Phase 4 — Constellation Processing & FEC  *(~2 weeks)*

Goal: Decoded bits from LDPC. L1 signaling parsed. System fully self-configuring.

### 4.1 Constellation Demapper [~]
- [x] `lib/ofdm/constellation_demapper.h/.cc`
- [x] Soft LLR output for: QPSK, 16/64/256/1024/4096-QAM (uniform)
- [ ] NUC (non-uniform constellation) lookup tables for NUC-QAM variants per ATSC A/322 §7.5
- [ ] Load NUC tables from `config/atsc3_modes.json`
- [x] Output: `int8_t` LLRs (clamped ±127)
- [x] Unit test: QPSK, AWGN SNR=10 dB → LLR sign correct > 99.9%

### 4.2 De-interleavers [x]
- [x] `lib/ofdm/cell_deinterleaver.h/.cc` — per ATSC A/322 §8.1
- [x] `lib/ofdm/time_deinterleaver.h/.cc` (TDI) — convolutional; configurable depth
- [x] `lib/ofdm/freq_deinterleaver.h/.cc` (FDI) — per ATSC A/322 §8.3
- [x] Unit test for each: apply interleaver (reference Python), verify C++ de-interleaver round-trips

### 4.3 LDPC Decoder [~]
- [x] `lib/fec/ldpc_decoder.h/.cc`
- [x] Min-sum belief propagation; configurable iteration count (default 50)
- [ ] Parity check matrices for all ATSC 3.0 code rates (2/15 through 13/15) as sparse arrays in `config/ldpc_tables/`
- [x] Both codeword lengths: 64800 bits and 16200 bits
- [x] No GR/UHD/FFTW3 dependency; builds standalone
- [x] AXI4-S: TDATA=int8(LLR), TLAST=codeword boundary
- [ ] Performance target: ≥ 1 Mb/s throughput on CI runner (single thread)
- [x] Unit test: encode with reference encoder → decode all-zero codeword; BER=0 above waterfall
- [ ] Fixed-point equivalence test

### 4.4 BCH Decoder [x]
- [x] `lib/fec/bch_decoder.h/.cc` — GF(2^16), t=12 error correction
- [x] Uses LDPC output as input; corrects residual errors
- [x] Unit test: inject 6 bit errors → all corrected; 13 errors → failure flagged

### 4.5 L1 Preamble Decoder
- [ ] `lib/framing/l1_decoder.h/.cc`
- [ ] L1-Pre parsing (bootstrap payload): FFT size, CP length, L1-Post size/modulation
- [ ] L1-Post parsing: PLP count, modulation, code rate, interleaver config per PLP
- [ ] Populates `Atsc3Config` struct; broadcasts to all downstream blocks via observer pattern
- [ ] Unit test: known L1 bits (from ATSC A/322 §5 example) → correct config struct

### 4.6 Dynamic Reconfiguration [~]
- [x] `Atsc3Config` struct with all runtime parameters
- [ ] `ConfigBus` (simple pub/sub, no dynamic alloc) wiring L1 decoder output to all dependent blocks
- [ ] Integration test: FileSource with real ATSC 3.0 capture → L1 parses correctly, blocks reconfigure

---

## Phase 5 — Transport & Audio/Video  *(~1.5 weeks)*

Goal: Live A/V decode and playback from real broadcast.

### 5.1 ALP Demultiplexer
- [ ] `lib/framing/alp_demux.h/.cc`
- [ ] ALP header parsing (ATSC A/330)
- [ ] IP datagram reassembly from ALP packets
- [ ] PLP demultiplexing; output per-PLP byte streams
- [ ] Unit test: synthetic ALP packet sequence → correct datagram reassembly

### 5.2 ROUTE/DASH Parser
- [ ] `lib/framing/route_parser.h/.cc`
- [ ] ROUTE session announcement (SLT, LCT) parsing
- [ ] DASH segment URL resolution
- [ ] `lib/framing/service_catalog.h/.cc` — list of available services per transport session
- [ ] Unit test: reference ROUTE SLT XML → service list populated correctly

### 5.3 FFmpeg A/V Decoder
- [ ] `av/hevc_decoder.h/.cc` — libavcodec HEVC ES → raw YUV420 frames
- [ ] `av/ac4_decoder.h/.cc` — libavcodec AC-4 / HE-AAC ES → PCM float32
- [ ] Thread-safe output queue (fixed-size ring buffer, no dynamic alloc in steady state)
- [ ] Unit test: known 1-second HEVC ES → correct number of frames decoded; no segfaults

### 5.4 GStreamer Playback Pipeline
- [ ] `av/gst_player.h/.cc`
- [ ] `appsrc → h265parse → avdec_h265 → videoconvert → autovideosink`
- [ ] `appsrc → aacparse → avdec_aac → audioconvert → autoaudiosink`
- [ ] A/V sync via GStreamer pipeline clock
- [ ] GStreamer pipeline must be created on main thread (documented constraint)
- [ ] Integration test: synthetic H.265 Annex B + AAC ES → pipeline runs without error for 5 s

### 5.5 End-to-End Integration Test
- [ ] Integration test: FileSource (real ATSC 3.0 IQ capture, git-lfs) → full pipeline → decoded frame count > 0
- [ ] This is the **MVP gate test** — must pass before Phase 6

---

## Phase 6 — Metrics, Scanner & GNU Radio Wrappers  *(~1 week)*

Goal: Complete MVP. Observable signal quality, channel scanner, working GRC flowgraph.

### 6.1 Signal Quality Metrics
- [ ] `lib/metrics/snr_estimator.cc` — decision-directed from pilot residuals
- [ ] `lib/metrics/mer_estimator.cc` — MER from equalized constellation RMS
- [ ] `lib/metrics/ber_estimator.cc` — pre-FEC BER proxy from LDPC iteration count
- [ ] `lib/metrics/signal_strength.cc` — dBm from HAL RSSI + AGC gain correction
- [ ] JSON metrics output struct: `{ "rssi_dbm", "snr_db", "mer_db", "ber_pre_fec", "ldpc_converged", "services" }`

### 6.2 GNU Radio OOT Blocks
- [ ] GR block for each `lib/` stage: `atsc3_bootstrap_detect`, `atsc3_ofdm_demod`, `atsc3_channel_eq`, `atsc3_fec_decode`, `atsc3_alp_demux`
- [ ] Each block: AXI4-S contract comment in header, delegates immediately to `lib/` class
- [ ] `cmake/gr_python_check.cmake` — detects GR 3.8 vs 3.10 and adjusts block registration API
- [ ] GRC `.yml` block definition files for all blocks

### 6.3 Channel Scanner
- [ ] `apps/scanner.py` — sweeps US channel plan from `config/channel_plan_us.json`
- [ ] Per channel: tune → acquire 2 s → report `{ channel, freq_hz, rssi_dbm, locked, mer_db, services[] }`
- [ ] Output modes: table (default), JSON (`--format json`), CSV (`--format csv`)
- [ ] Configurable dwell time (`--dwell 2.0`), band subset (`--band uhf|vhf|all`), single channel (`--channel N`)

### 6.4 GRC Flowgraph
- [ ] `apps/atsc3_rx.grc` — USRP Source (via HAL) → full decode chain → QT GUI metrics sink
- [ ] Parameters exposed in GRC: frequency, gain, sample_rate, output_file (optional)
- [ ] Works with FileSource for offline decode (parameter to switch source)

### 6.5 Metrics HTTP Server
- [ ] `apps/metrics_server.py` — Flask/aiohttp endpoint at `/metrics` returning JSON
- [ ] Refresh rate: 1 Hz
- [ ] Simple HTML dashboard at `/` (plain tables, no JS framework)

### 6.6 MVP Documentation Pass
- [ ] `README.md` §Usage section verified against actual build
- [ ] Doxygen comment coverage ≥ 80% of public headers (enforced in `nightly.yml`)
- [ ] `CHANGELOG.md` entry for v0.1.0
- [ ] Git tag `v0.1.0-mvp`, release workflow fires

---

## Phase 7 — Hardening, Compliance & Performance  *(post-MVP, ongoing)*

### 7.1 Full ATSC 3.0 H Matrix Support
- [ ] Generate all LDPC H matrices per ATSC A/322 §12.2 for both codeword lengths (64800, 16200)
- [ ] Validate H matrices for all 12 code rates: 2/15, 3/15, 4/15, 5/15, 6/15, 7/15, 8/15, 9/15, 10/15, 11/15, 12/15, 13/15
- [ ] Store H matrices in sparse CSR format in `config/ldpc_tables/` (row indices, column indices per rate/length)
- [ ] Add H matrix loader with runtime selection based on L1 signaling
- [ ] Unit test: verify H matrix dimensions and sparsity match ATSC spec tables
- [ ] Unit test: syndrome check with known codewords for each code rate

### 7.2 Interleaver Optimizations
- [ ] Cell deinterleaver: replace modular arithmetic with bit-masking for power-of-2 sizes
- [ ] Time deinterleaver: implement double-buffered memory access pattern for cache efficiency
- [ ] Frequency deinterleaver: precompute permutation tables at init (eliminate runtime address calculation)
- [ ] SIMD vectorization for interleaver copy loops (`-mavx2` / `-msse4.2`)
- [ ] Benchmark: measure cycles/cell for each interleaver; target < 10 cycles/cell
- [ ] Memory layout optimization: ensure deinterleaver buffers are 64-byte aligned for cache line efficiency

### 7.3 ATSC 3.0 Compliance Testing
- [ ] Conformance test suite against ATSC A/322 reference vectors (obtain from ATSC or implement generator)
- [ ] Bootstrap detection: verify all 128 bootstrap symbol variants decode correctly
- [ ] L1 signaling: verify L1-Pre and L1-Post CRC checks pass for all valid configurations
- [ ] LDPC: verify BER vs Eb/N0 waterfall curves match ATSC spec Figure 12.x within 0.1 dB
- [ ] NUC constellations: verify all NUC tables match ATSC A/322 §7.5 exactly
- [ ] Interleaver round-trip: verify bit-exact match with ATSC reference interleaver for all modes
- [ ] Document compliance status in `docs/compliance.md` with pass/fail matrix per ATSC requirement

### 7.4 Performance Profiling & Optimization
- [ ] Profile with `perf` / `gprof`; identify bottleneck block
- [ ] LDPC: vectorize min-sum inner loop with SIMD intrinsics (`-mavx2`)
- [ ] FFT: evaluate FFTW plan modes (`FFTW_MEASURE` vs `FFTW_PATIENT`) for target host
- [ ] Multi-PLP support (currently single PLP decoded)
- [ ] Robustness: restart-on-lock-loss without flowgraph teardown
- [ ] Memory: valgrind/ASAN clean run on full pipeline
- [ ] Fuzzing: libFuzzer on ALP and ROUTE parsers

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
