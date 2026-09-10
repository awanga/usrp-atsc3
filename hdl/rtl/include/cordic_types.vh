// cordic_types.vh — Shared constants for hdl/rtl/common/cordic.v
//
// IEEE 1364-2001 Verilog. Mirrors lib/dsp/cordic.h's mode selection and
// kCordicIterations/kCordicInvGainQ15 constants exactly; see cordic.v for
// the iterative dual-mode core itself and hdl/rtl/sync/bootstrap_detector.v
// for the first consumer.

`ifndef CORDIC_TYPES_VH
`define CORDIC_TYPES_VH

`define CORDIC_MODE_ROTATE 1'b0
`define CORDIC_MODE_VECTOR 1'b1

// Matches lib/dsp/cordic.h::kCordicIterations exactly -- iterations 14/15
// are structural no-ops in the Q1.15 angle format (atan(2^-14)/pi and
// atan(2^-15)/pi both truncate to 0 there), so this is not an arbitrary
// round number; see that header's comment for the derivation.
`define CORDIC_ITERATIONS 14

// 1/K, K = CORDIC pseudo-rotation gain (~1.6468). Rotation mode's x0 reset
// value; vectoring mode's magnitude-rescale constant. Matches
// lib/dsp/cordic.h::kCordicInvGainQ15 exactly.
`define CORDIC_INV_GAIN_Q15 16'sd19898

`endif // CORDIC_TYPES_VH
