# Changelog

All notable changes to gr-atsc3 will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-08-07

### Release Notes

This is the first stable release of gr-atsc3, a complete ATSC 3.0 physical-layer
receiver for GNU Radio. The receiver has been validated with live over-the-air
broadcasts using USRP N210 + TVRX2 at 6.25 MS/s with no dropped packets.

### Added (since 0.1.0-mvp)

#### NUC Constellation Support
- Full ATSC A/322 Section 7.5 Non-Uniform Constellation (NUC) tables
- 2D-NUC support for 16, 64, and 256-QAM with quadrant expansion
- 1D-NUC support for 1024 and 4096-QAM with I/Q symmetry
- Unit average power normalization per specification

#### Enhanced Receiver Features
- L1 signaling metrics dashboard with real-time display
- Service recording capability for transport stream capture
- Closed caption (CEA-708) extraction utility
- Emergency Alert System (EAS) monitoring

#### Capture & Validation Utilities
- `apps/capture_iq.py` — IQ capture with ATSC 3.0 signal validation
- `apps/validate_route.py` — ROUTE/ALP content verification
- End-to-end offline test (`test/integration/e2e_offline_test.py`)

#### Testing
- 652 total tests (unit + integration + compliance)
- End-to-end live USRP validation at 6.25 MS/s
- 10-second sustained streaming without packet drops

### Changed
- Improved bootstrap detection reliability (threshold 0.6)
- Auto-compute active carriers from FFT size in frequency deinterleaver
- Relaxed PAPR threshold in signal power tests for measurement tolerance

### Fixed
- Bootstrap detection position calculation (peaks at symbol end)
- Cell/frequency interleaver bijection for non-power-of-2 sizes
- NUC demapper LLR scaling for normalized constellations

## [0.1.0-mvp] - 2026-05-24

### Added

#### Hardware Abstraction Layer
- `IQSource` interface with `UHDSource`, `FileSource`, and `NullSource` implementations
- USRP N2x0 with TVRX/TVRX2 daughterboard support (UHD 3.15+)
- AGC controller with configurable attack/release time constants
- RSSI reading from TVRX sensor API

#### OFDM Front-End
- Bootstrap detector using Schmidl-Cox autocorrelation (4K FFT)
- Frame timing recovery with Gardner TED and polyphase interpolator
- Cyclic prefix removal for all ATSC 3.0 CP fractions
- FFT engine supporting 8K/16K/32K transforms (FFTW3 backend)
- Pilot extraction for scattered pilots (PP1-PP8), continual pilots, and edge pilots
- Coarse and fine CFO correction

#### Channel Estimation & Equalization
- Least-squares channel estimator at pilot positions
- Wiener interpolation with configurable tap counts and channel profiles
- ZF and MMSE frequency-domain equalizers
- Phase noise tracking from continual pilots

#### Forward Error Correction
- Constellation demapper for QPSK through 4096-QAM (uniform and NUC)
- Cell, time, and frequency de-interleavers per ATSC A/322
- LDPC decoder (min-sum BP) for all 12 code rates, both codeword lengths
- BCH decoder with t=12 error correction

#### Framing & Transport
- L1-Pre and L1-Post preamble decoder
- Dynamic reconfiguration via ConfigBus pub/sub
- ALP demultiplexer with IP datagram reassembly
- ROUTE/DASH session parser with service catalog

#### Audio/Video Decode
- HEVC video decoder via FFmpeg libavcodec
- AC-4/HE-AAC audio decoder via FFmpeg
- GStreamer playback pipeline with A/V sync

#### Signal Quality Metrics
- SNR estimator from pilot residuals
- MER/EVM estimator from equalized constellation
- BER proxy from LDPC iteration count
- Signal strength with AGC gain correction
- JSON metrics aggregator

#### GNU Radio Integration
- OOT blocks: `atsc3_bootstrap_detect`, `atsc3_ofdm_demod`, `atsc3_channel_eq`, `atsc3_fec_decode`, `atsc3_alp_demux`
- GRC block definition files (.yml) for GNU Radio Companion
- Example flowgraph: `apps/atsc3_rx.grc`

#### Applications
- Channel scanner (`apps/scanner.py`) with table/JSON/CSV output
- Metrics HTTP server (`apps/metrics_server.py`) with HTML dashboard

#### Testing & CI
- Unit tests covering all lib/ components
- Integration tests for transport layer
- CI pipeline with clang-format and cppcheck
- Test IQ capture from live ATSC 3.0 broadcast

### Architecture Notes
- All DSP code in `lib/` has zero GNU Radio or UHD dependencies
- Fixed-point build path (`ATSC3_FIXED_POINT=ON`) for future FPGA RTL port
- AXI4-Stream interface contracts documented in all block headers
- No dynamic allocation after initialization

## [Unreleased]

### Planned for v1.1.0
- Multi-PLP simultaneous decode
- LDPC SIMD optimization (AVX2/AVX-512)
- External H-matrix loading from JSON
- Code-rate-dependent NUC tables

### Post-MVP Milestones
- ML-based channel estimation (ONNX Runtime backend)
- FPGA RTL port (Verilator + cocotb testbenches)
- Fixed-point numerical equivalence verification
