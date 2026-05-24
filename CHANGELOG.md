# Changelog

All notable changes to gr-atsc3 will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
- 518 unit tests covering all lib/ components
- 14 integration tests for transport layer
- CI pipeline with clang-format and cppcheck
- Test IQ capture from live ATSC 3.0 broadcast

### Architecture Notes
- All DSP code in `lib/` has zero GNU Radio or UHD dependencies
- Fixed-point build path (`ATSC3_FIXED_POINT=ON`) for future FPGA RTL port
- AXI4-Stream interface contracts documented in all block headers
- No dynamic allocation after initialization

### Known Limitations
- Single PLP decode only (multi-PLP deferred to v0.2)
- NUC constellations use uniform QAM approximation for 64-QAM and above
- Fixed-point equivalence tests not yet implemented
- Branch protection not yet configured on GitHub

## [Unreleased]

### Planned for v0.2.0
- Full ATSC A/322 NUC constellation tables
- Multi-PLP simultaneous decode
- LDPC SIMD optimization (AVX2)
- External H-matrix loading from JSON

### Post-MVP Milestones
- ML-based channel estimation (ONNX Runtime backend)
- FPGA RTL port (Verilator + cocotb testbenches)
