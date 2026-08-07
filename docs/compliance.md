# ATSC 3.0 Compliance Status

This document tracks gr-atsc3 compliance with ATSC A/322:2023 (Physical Layer Protocol).

## Compliance Summary

| Component | Status | Test Coverage | Notes |
|-----------|--------|---------------|-------|
| Bootstrap Detection | Implemented | 128 variants | Schmidl-Cox autocorrelation |
| L1-Pre Signaling | Implemented | All fields | CRC-32 validation |
| L1-Post Signaling | Implemented | Multi-PLP | CRC-32 validation |
| OFDM Demodulation | Implemented | 8K/16K/32K FFT | FFTW3 backend |
| Pilot Extraction | Implemented | PP1-PP8 | All scatter patterns |
| Channel Estimation | Implemented | LS + Wiener | SNR estimation |
| Constellation Demapping | Partial | Uniform QAM | NUC tables placeholder |
| Cell Interleaving | Implemented | Round-trip tested | Bit-reversal permutation |
| Time Interleaving | Implemented | CTI/HTI modes | Convolutional |
| Frequency Interleaving | Implemented | Per-FFT tables | LFSR-based |
| LDPC Decoding | Implemented | All 12 rates | Min-sum BP, 16K/64K |
| BCH Decoding | Implemented | t=12 | GF(2^16) |
| ALP Demux | Implemented | IP reassembly | Per-PLP streams |

---

## Detailed Compliance Matrix

### Section 5: Bootstrap Signal (ATSC A/322 Section 5)

| Requirement | Status | Test | Notes |
|-------------|--------|------|-------|
| 4096-sample bootstrap | Pass | test_bootstrap_variants | Fixed length per spec |
| 128 variant detection | Pass | test_bootstrap_variants | All major/minor/PN combinations |
| CFO estimation | Pass | test_bootstrap_variants | Within tolerance |
| SNR estimation | Pass | test_bootstrap_detector | Decision-directed |
| Detection threshold | Pass | test_bootstrap_variants | Configurable, default 0.7 |
| False alarm rate | Pass | test_bootstrap_variants | Acceptable in noise-only |

### Section 5: L1 Signaling (ATSC A/322 Section 5.3-5.4)

| Requirement | Status | Test | Notes |
|-------------|--------|------|-------|
| L1-Pre parsing (200 bits) | Pass | test_l1_compliance | All fields extracted |
| L1-Pre CRC-32 | Pass | test_l1_compliance | ISO 3309 polynomial |
| FFT size field (2 bits) | Pass | test_l1_compliance | 8K/16K/32K |
| GI fraction field (4 bits) | Pass | test_l1_compliance | 11 valid values |
| Pilot pattern field (3 bits) | Pass | test_l1_compliance | PP1-PP8 |
| L1-Post size field (16 bits) | Pass | test_l1_compliance | Full range |
| L1-Post modulation (4 bits) | Pass | test_l1_compliance | All uniform QAM |
| L1-Post code rate (4 bits) | Pass | test_l1_compliance | 12 code rates |
| L1-Post FEC type (2 bits) | Pass | test_l1_compliance | Short/Long LDPC |
| Num subframes (8 bits) | Pass | test_l1_compliance | 1-255 |
| L1-Post parsing | Pass | test_l1_decoder | Multi-PLP support |
| L1-Post CRC-32 | Pass | test_l1_decoder | Per-block CRC |
| PLP configuration | Pass | test_l1_decoder | Up to 64 PLPs |

### Section 7: OFDM Structure (ATSC A/322 Section 7)

| Requirement | Status | Test | Notes |
|-------------|--------|------|-------|
| 8K FFT (8192 points) | Pass | test_fft_engine | 6913 active carriers |
| 16K FFT (16384 points) | Pass | test_fft_engine | 13825 active carriers |
| 32K FFT (32768 points) | Pass | test_fft_engine | 27649 active carriers |
| CP lengths (11 values) | Pass | test_cp_removal | Per Table 7.1 |
| Scattered pilots PP1-PP8 | Pass | test_pilot_extractor | All patterns |
| Continual pilots | Pass | test_pilot_extractor | Edge + scattered |
| Edge pilots | Pass | test_pilot_extractor | DC and band edges |

### Section 7.5: Constellations (ATSC A/322 Section 7.5)

| Requirement | Status | Test | Notes |
|-------------|--------|------|-------|
| QPSK | Pass | test_constellation_demapper | Gray-coded |
| 16-QAM | Pass | test_constellation_demapper | Gray-coded |
| 64-QAM | Pass | test_constellation_demapper | Gray-coded |
| 256-QAM | Pass | test_constellation_demapper | Gray-coded |
| 1024-QAM | Pass | test_constellation_demapper | Gray-coded |
| 4096-QAM | Pass | test_constellation_demapper | Gray-coded |
| NUC-16 (code-rate dependent) | Partial | test_nuc_compliance | Placeholder tables |
| NUC-64 (code-rate dependent) | Partial | test_nuc_compliance | Placeholder tables |
| NUC-256 (code-rate dependent) | Partial | test_nuc_compliance | Placeholder tables |
| NUC-1024 (code-rate dependent) | Partial | test_nuc_compliance | Placeholder tables |
| Soft LLR output | Pass | test_constellation_demapper | Max-log approximation |

### Section 8: Interleaving (ATSC A/322 Section 8)

| Requirement | Status | Test | Notes |
|-------------|--------|------|-------|
| Cell interleaving | Pass | test_interleaver_compliance | Bit-reversal permutation |
| Cell de-interleaving round-trip | Pass | test_interleaver_compliance | Verified bijection |
| Time interleaving (CTI) | Pass | test_interleaver_compliance | Convolutional, depth 0-15 |
| Time interleaving (HTI) | Pass | test_time_deinterleaver | Hybrid mode |
| Time interleaving settling | Pass | test_time_deinterleaver | depth blocks |
| Frequency interleaving (8K) | Pass | test_interleaver_compliance | 6913 carriers |
| Frequency interleaving (16K) | Pass | test_interleaver_compliance | 13825 carriers |
| Frequency interleaving (32K) | Pass | test_interleaver_compliance | 27649 carriers |
| Frequency de-interleaving round-trip | Pass | test_interleaver_compliance | Verified bijection |

### Section 12: LDPC Coding (ATSC A/322 Section 12)

| Requirement | Status | Test | Notes |
|-------------|--------|------|-------|
| Short codeword (16200 bits) | Pass | test_ldpc_decoder | Q=90 expansion |
| Long codeword (64800 bits) | Pass | test_ldpc_decoder | Q=360 expansion |
| Rate 2/15 | Pass | test_ldpc_matrix | H matrix validated |
| Rate 3/15 | Pass | test_ldpc_matrix | H matrix validated |
| Rate 4/15 | Pass | test_ldpc_matrix | H matrix validated |
| Rate 5/15 | Pass | test_ldpc_waterfall | Threshold tested |
| Rate 6/15 | Pass | test_ldpc_matrix | H matrix validated |
| Rate 7/15 | Pass | test_ldpc_waterfall | Threshold tested |
| Rate 8/15 | Pass | test_ldpc_matrix | H matrix validated |
| Rate 9/15 | Pass | test_ldpc_matrix | H matrix validated |
| Rate 10/15 | Pass | test_ldpc_waterfall | Threshold tested |
| Rate 11/15 | Pass | test_ldpc_matrix | H matrix validated |
| Rate 12/15 | Pass | test_ldpc_matrix | H matrix validated |
| Rate 13/15 | Pass | test_ldpc_matrix | H matrix validated |
| Min-sum decoding | Pass | test_ldpc_decoder | Configurable iterations |
| Waterfall curves | Partial | test_ldpc_waterfall | Simplified Monte Carlo |

### Section 12.3: BCH Coding (ATSC A/322 Section 12.3)

| Requirement | Status | Test | Notes |
|-------------|--------|------|-------|
| BCH(16383, 16215) | Pass | test_bch_decoder | GF(2^14), t=12 |
| 12-error correction | Pass | test_bch_decoder | Verified |
| 13+ error detection | Pass | test_bch_decoder | Failure flagged |

---

## Test Execution

Run compliance tests with:

```bash
# All compliance tests
ctest -L compliance --output-on-failure

# Specific compliance test
./build/test/compliance/test_bootstrap_variants
./build/test/compliance/test_l1_compliance
./build/test/compliance/test_interleaver_compliance
./build/test/compliance/test_nuc_compliance
./build/test/compliance/test_ldpc_waterfall
```

---

## Known Limitations

### NUC Constellations (Partial)

The current implementation uses placeholder NUC tables. Full compliance requires:

1. Loading ATSC A/322 Table 7.5 NUC coordinates for each code rate
2. Creating `config/nuc_tables.json` with all NUC-16/64/256/1024 tables
3. Code-rate-dependent NUC selection in `ConstellationDemapper`

**Impact**: Uniform QAM fallback provides correct functionality but suboptimal performance at low SNR.

### LDPC Waterfall Curves (Partial)

Current waterfall tests use simplified Monte Carlo simulation:

1. Full ATSC compliance requires comparison to official reference curves
2. Current tests verify general waterfall behavior, not exact dB thresholds
3. Additional test vectors from ATSC would improve confidence

**Impact**: Decoder functions correctly; exact threshold verification is approximate.

---

## Reference Documents

- **ATSC A/322:2023**: Physical Layer Protocol
- **ATSC A/330:2023**: Link-Layer Protocol (ALP)
- **ATSC A/331:2023**: Signaling, Delivery, Synchronization, and Error Protection

---

## Revision History

| Date | Version | Changes |
|------|---------|---------|
| 2026-07-03 | 0.1.0 | Initial compliance documentation |
