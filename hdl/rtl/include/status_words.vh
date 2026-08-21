// status_words.vh — Bit-packed layouts for fixed-size sideband/status
// structs ported from lib/. IEEE 1364-2001 Verilog (`define only, same
// reasoning as axi4s_types.vh: top-level `parameter` isn't legal outside a
// module body).
//
// Field packing convention: LSB-first, each field individually byte-aligned
// (no sub-byte packing across field boundaries except explicit 1-bit flags,
// which own a full reserved byte). Every complex<int16_t> field packs as
// {re[15:0], im[15:0]} (re = upper half, im = lower half), matching the
// existing hdl/stubs/README.md data-type table. See
// hdl/docs/wire_level_layouts.md for the full rationale, the fields that
// are deliberately NOT included here (log10-dependent, out of scope this
// milestone), and why DemapResult and L1Pre/L1Post have no entry in this
// file (they don't need one -- see that doc).

`ifndef STATUS_WORDS_VH
`define STATUS_WORDS_VH

`include "axi4s_types.vh"

//------------------------------------------------------------------------------
// PilotSymbol (lib/ofdm/pilot_extractor.h:46-57) -- Phase 9.5 sideband
//------------------------------------------------------------------------------

`define PILOT_SYMBOL_WIDTH 96

`define PILOT_SYMBOL_SUBCARRIER_INDEX_HI 15
`define PILOT_SYMBOL_SUBCARRIER_INDEX_LO 0

`define PILOT_SYMBOL_REFERENCE_IM_HI 31
`define PILOT_SYMBOL_REFERENCE_IM_LO 16
`define PILOT_SYMBOL_REFERENCE_RE_HI 47
`define PILOT_SYMBOL_REFERENCE_RE_LO 32

`define PILOT_SYMBOL_RECEIVED_IM_HI 63
`define PILOT_SYMBOL_RECEIVED_IM_LO 48
`define PILOT_SYMBOL_RECEIVED_RE_HI 79
`define PILOT_SYMBOL_RECEIVED_RE_LO 64

`define PILOT_SYMBOL_TYPE_HI 87
`define PILOT_SYMBOL_TYPE_LO 80
`define PILOT_TYPE_SCATTERED 8'd0
`define PILOT_TYPE_CONTINUAL 8'd1
`define PILOT_TYPE_EDGE      8'd2
// bits [95:88] reserved, drive 0

//------------------------------------------------------------------------------
// BootstrapDetection (lib/sync/bootstrap_detector.h:23-40) -- Phase 9.1 status
//
// snr_db is NOT included: lib/sync/bootstrap_detector.cc computes it via
// std::log10, which Phase 9.0b's CORDIC work does not cover this milestone
// (rotation/vectoring modes only -- log10 needs hyperbolic mode, a separate
// design). Same exclusion class as the metrics registers in
// config/hdl_register_map.json.
//------------------------------------------------------------------------------

`define BOOTSTRAP_DETECTION_WIDTH 104

`define BOOTSTRAP_DETECTION_DETECTED_BIT 0
// bits [7:1] reserved, drive 0

`define BOOTSTRAP_DETECTION_SAMPLE_INDEX_HI 55
`define BOOTSTRAP_DETECTION_SAMPLE_INDEX_LO 8
// 48 bits: wraps after 2^48 samples (~1.4 years continuous at 6.25 MS/s).
// Deliberately wider than a natural 32-bit counter, which would wrap in
// ~11.5 minutes at 6.25 MS/s -- flagged here so a future width reduction
// isn't made without re-deriving this bound.

`define BOOTSTRAP_DETECTION_CFO_HZ_HI 87
`define BOOTSTRAP_DETECTION_CFO_HZ_LO 56
// Signed Hz, matches config/hdl_register_map.json's freq_correction
// INITIAL_CFO_HZ register format (int32) -- this is already the final Hz
// conversion done in C++ (phase * sample_rate_hz / (2*pi*L)), not a raw
// CORDIC angle, so it does not depend on Phase 9.0b's angle convention.

`define BOOTSTRAP_DETECTION_METRIC_HI 103
`define BOOTSTRAP_DETECTION_METRIC_LO 88
// q1_15, same format/truncation convention as config/hdl_register_map.json.

//------------------------------------------------------------------------------
// FrameEvent (lib/sync/frame_sync.h:90-103) -- Phase 9.6 status.
// Belongs to frame_sync, not timing_recovery (corrected in the HDL port
// plan from an earlier draft).
//------------------------------------------------------------------------------

`define FRAME_EVENT_WIDTH 136

`define FRAME_EVENT_TYPE_HI 7
`define FRAME_EVENT_TYPE_LO 0
`define FRAME_EVENT_TYPE_FRAME_START    8'd0
`define FRAME_EVENT_TYPE_SUBFRAME_START 8'd1
`define FRAME_EVENT_TYPE_SYMBOL_START   8'd2

`define FRAME_EVENT_SAMPLE_INDEX_HI 55
`define FRAME_EVENT_SAMPLE_INDEX_LO 8
// Same 48-bit / wraparound reasoning as BOOTSTRAP_DETECTION_SAMPLE_INDEX.

`define FRAME_EVENT_FRAME_NUMBER_HI 87
`define FRAME_EVENT_FRAME_NUMBER_LO 56

`define FRAME_EVENT_SUBFRAME_NUMBER_HI 103
`define FRAME_EVENT_SUBFRAME_NUMBER_LO 88

`define FRAME_EVENT_SYMBOL_NUMBER_HI 119
`define FRAME_EVENT_SYMBOL_NUMBER_LO 104

`define FRAME_EVENT_CONFIDENCE_HI 135
`define FRAME_EVENT_CONFIDENCE_LO 120
// q1_15. lib/sync/frame_sync.cc's normalization is sqrt-based (Phase 9.0b's
// CORDIC vectoring mode covers this -- unlike BootstrapDetection's snr_db,
// nothing here depends on the not-yet-covered log10/hyperbolic case.

//------------------------------------------------------------------------------
// Absolute <-> relative subcarrier indexing (pinned once, applies wherever
// PILOT_SYMBOL_SUBCARRIER_INDEX meets a relative-indexed on-chip array):
//
//   first_active_carrier = (fft_size - num_active_carriers) >> 1
//   relative_index        = absolute_index - first_active_carrier
//
// fft_size/num_active_carriers both come from the l1_status register bank
// (config/hdl_register_map.json) -- first_active_carrier is not itself a
// register; every block computes it internally with the one-line formula
// above (matches lib/ofdm/pilot_extractor.cc's compute_first_active()).
// received[]/channel_estimate[]/equalized/LLR streams are positional: beat
// N on those streams IS relative subcarrier index N, with no explicit index
// field on the wire. Only PILOT_SYMBOL carries an explicit index, and it is
// always absolute, matching lib/ofdm/pilot_extractor.h's own comment ("FFT
// bin index").
//------------------------------------------------------------------------------

`endif // STATUS_WORDS_VH
