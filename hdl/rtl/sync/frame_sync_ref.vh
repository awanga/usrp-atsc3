// SPDX-License-Identifier: CERN-OHL-W-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// frame_sync_ref.vh -- Preamble reference table, one period
//
// GENERATED FILE -- do not hand-edit. Produced from the running model
// (lib/sync/frame_sync.cc init_preamble_reference(), ATSC3_FIXED_POINT=ON):
//
//   echo Z | build-fxp/hdl/sim/golden/frame_sync_gen
//
// then reformatting each "Z <k> <re> <im>" line below. The model's
// reference is the placeholder sequence cos/sin(2*pi*((7k) mod 13)/13)/2,
// so it repeats every 13 samples (frame_sync_gen refuses to dump it if it
// does not). Included inside a function body in frame_correlator.v;
// regenerate whenever init_preamble_reference() changes. test_frame_correlator.py
// fails if this file drifts from the model.

case (ph)
    4'd0: frame_ref = {16'sd16384, 16'sd0};
    4'd1: frame_ref = {-16'sd15907, -16'sd3920};
    4'd2: frame_ref = {16'sd14507, 16'sd7614};
    4'd3: frame_ref = {-16'sd12263, -16'sd10864};
    4'd4: frame_ref = {16'sd9307, 16'sd13483};
    4'd5: frame_ref = {-16'sd5809, -16'sd15319};
    4'd6: frame_ref = {16'sd1974, 16'sd16264};
    4'd7: frame_ref = {16'sd1974, -16'sd16264};
    4'd8: frame_ref = {-16'sd5809, 16'sd15319};
    4'd9: frame_ref = {16'sd9307, -16'sd13483};
    4'd10: frame_ref = {-16'sd12263, 16'sd10864};
    4'd11: frame_ref = {16'sd14507, -16'sd7614};
    4'd12: frame_ref = {-16'sd15907, 16'sd3920};
    default: frame_ref = 32'sd0;
endcase
