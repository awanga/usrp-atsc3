# ATSC 3.0 Compliance Status

What `lib/` implements from ATSC A/321:2026-06 (System Discovery and
Signaling) and A/322:2026-06 (Physical Layer Protocol), how it is tested,
and what is missing. A/322 specifies the transmitter; the receive side is
the inverse of A/322 Figures 4.1, 6.1 and 7.1.

Status values:

- **From spec**: written from the spec text; tests take their expected
  values from the spec or an independent model written from it, and are
  shown to fail on mutants.
- **Receiver design**: a receiver algorithm the spec does not define.
- **Partial** / **Not implemented**: see Notes.

No off-air capture has been decoded. `test/captures/` holds synthetic A/321
bootstraps in Gaussian noise (see its README), so nothing past the bootstrap
has an end-to-end conformance test.

## Summary

| Component | Spec | Status | Tests | Notes |
|-----------|------|--------|-------|-------|
| Bootstrap sequences and signaling | A/321 5-6; A/322 9.1, H.1 | From spec | test_bootstrap_signal | ZC/PN, Gray demap, fields, Table H.1.1 |
| Bootstrap detection | A/321 5.4 | Receiver design | test_bootstrap_detector, test_iq_replay | CAB repetition metric, fractional CFO |
| Bootstrap decoding | A/321 5.2-5.4, Annex A | From spec | test_bootstrap_decoder, test_iq_replay | Timing, integer CFO, fields, termination |
| Frame sync | A/321 6.1.1.1; A/322 7.2, 9.2 | From spec | test_frame_sync, test_iq_replay | Bootstrap-driven; time-aligned frame length only |
| Resampling to 6.144 / (N+16) x 0.384 MS/s | A/321 5.1; A/322 9.1 | Not implemented | — | N210 rates differ from both |
| Preamble demodulation | A/322 8, Annex H | Not implemented | — | Preamble FFT/GI and pilots not wired |
| L1-Basic / L1-Detail FEC | A/322 6.5 | From spec | test_l1_fec | Not wired to preamble cells |
| L1-Basic / L1-Detail fields | A/322 9.2, 9.3 | From spec | test_l1_signaling | Nothing fills `Atsc3Config` from them yet |
| OFDM FFT | A/322 8 | From spec | test_fft_engine | 2K (bootstrap), 8K/16K/32K |
| Guard interval removal | A/322 8.5, Tables 8.9, 9.13 | Partial | test_cp_removal | No time-aligned excess samples (8.5.1) |
| Pilots (scattered, continual, edge) | A/322 8.1, Annexes D, E | From spec | test_pilot_extractor | Data symbols; preamble pilots not done |
| Channel estimation, equalization | — | Receiver design | test_channel_estimator, test_equalizer | LS + Wiener; single-tap |
| Frequency de-interleaver | A/322 7.3 | From spec | test_freq_deinterleaver | |
| Time de-interleaver (CTI, HTI) | A/322 7.1.4, 7.1.5 | Partial | test_time_deinterleaver | No CDL (inter-subframe HTI) |
| Cell de-interleaver | A/322 7.1.5.2 | From spec | test_cell_deinterleaver | |
| Constellations, NUC | A/322 6.3, Annex C | From spec | test_constellation_demapper, test_nuc_compliance | Tables generated from Annex C |
| Bit de-interleaver | A/322 6.2 | From spec | test_bit_deinterleaver | |
| LDPC | A/322 6.1.3, Annex A | From spec | test_ldpc_matrix, test_ldpc_decoder, test_ldpc_waterfall | Min-sum; waterfall is a simplified Monte Carlo |
| BCH / CRC-32 outer code | A/322 6.1.2 | From spec | test_bch_decoder | |
| Baseband packets | A/322 5.2 | From spec | test_baseband_packet | Descrambling and header |
| ALP | A/330 | Not re-checked | test_alp_demux | Predates the spec review |
| ROUTE/DASH | A/331 | Not re-checked | test_route_parser | Predates the spec review |

## Receive chain wiring

The blocks above are tested one at a time. They are not yet joined into a
receiver: L1 decoding does not configure the data path, and the GNU Radio
blocks do not run the A/321/A/322 chain. `TASKS.md` tracks this work.

## Test execution

```bash
# Unit and integration tests (integration needs the git-lfs captures)
ctest -L 'unit|integration' --output-on-failure

# Compliance tests (NUC tables, LDPC waterfall)
ctest -L compliance --output-on-failure
```

## Reference documents

- ATSC A/321:2026-06, System Discovery and Signaling
- ATSC A/322:2026-06, Physical Layer Protocol
- ATSC A/330, Link-Layer Protocol (ALP)
- ATSC A/331, Signaling, Delivery, Synchronization, and Error Protection
