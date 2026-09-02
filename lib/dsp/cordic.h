#pragma once

// CORDIC — shared fixed-point rotation/vectoring core
//
// Q1.15-only utility (no ATSC3_FIXED_POINT branch): provides the integer
// shift-add primitives that back the Phase 9.0b fixed-point rewrites of
// bootstrap_detector, freq_correction, and frame_sync (rotation mode for
// the NCO's cos/sin, vectoring mode for magnitude/phase). No multiplier,
// no trig/sqrt library call -- shift, add, and table lookup only, so this
// is also the direct C++ reference for a future shared hdl/rtl/common/cordic.v.
//
// AXI4-S: not a streaming block itself; a combinational/pipelined function
// called from within the blocks that use it.
//
// Angle format ("Q1.15 turns-over-pi"): an int16_t where the full range
// [-32768, 32767] linearly covers [-pi, +pi) radians (value/32768 * pi),
// the same truncating-saturating convention as float_to_q15()/q15_to_float()
// in types.h (+pi is not representable, matching the existing +1.0 caveat).
// This is not an arbitrary choice: adding exactly 0x8000 to any int16_t
// angle, using well-defined unsigned wraparound, rotates it by exactly one
// half turn (+-pi) mod 2*pi -- see add_half_turn() below. That is the whole
// quadrant-correction mechanism this file needs for full-range rotation and
// vectoring, and in RTL it is a single adder (or literally just an XOR of
// the sign bit), not a compare-and-branch. Resolves the "angle_tbd" format
// left open in config/hdl_register_map.json's PHASE_TRACKER_UNWRAP_THRESHOLD.

#include <cstdint>

namespace atsc3 {
namespace dsp {

// Number of micro-rotation iterations. Not 16: atan(2^-14)/pi and
// atan(2^-15)/pi both truncate to 0 in this Q1.15 angle format (see the
// atan table in cordic.cc), so iterations 14 and 15 are structural no-ops
// -- they'd rotate by a table entry of exactly 0. Trimmed rather than kept
// "for round numbers," since a future RTL pipeline would otherwise carry
// two dead stages.
constexpr int kCordicIterations = 14;

// 1/K where K = prod(sqrt(1 + 2^-2i)) for i in [0, kCordicIterations) is
// the CORDIC pseudo-rotation gain (~1.6468). Rotation mode pre-scales by
// this so its (cos, sin) output is already unity-gain; vectoring mode
// post-scales its magnitude output by the same constant. Exposed for
// tests/reference; RTL would hardcode this as the x0 reset value.
constexpr int16_t kCordicInvGainQ15 = 19898;  // float_to_q15(1/K) truncated

// Convert a float angle in radians to the Q1.15 angle format described
// above. Saturates like float_to_q15() (clamped to [-32767, 32767]).
int16_t float_to_q15_angle(float radians);

// Inverse of float_to_q15_angle().
float q15_angle_to_float(int16_t angle);

// Add exactly one half turn (+-pi) to an angle via well-defined unsigned
// wraparound. See the file header comment for why this is the whole
// quadrant-correction mechanism CORDIC needs here.
int16_t cordic_add_half_turn(int16_t angle);

struct CordicRotationResult {
    int16_t cos_theta;  // Q1.15, true (unity-gain) cos(theta)
    int16_t sin_theta;  // Q1.15, true (unity-gain) sin(theta)
};

// Rotation mode: (cos(theta), sin(theta)) for theta in the Q1.15 angle
// format, full range. Backs freq_correction's NCO (Phase 9.0b).
CordicRotationResult cordic_rotate(int16_t theta_q15_angle);

struct CordicVectorResult {
    // Q1.15-scaled magnitude of (x, y), but widened to int32_t: two
    // Q1.15 rails up to +-32767 each can have a true magnitude up to
    // ~46341, which does not fit in int16_t. Callers saturate/rescale to
    // their own block's expected range rather than this function
    // silently clipping a value it has no context to judge.
    int32_t magnitude;
    int16_t angle_q15;  // atan2(y, x) in the Q1.15 angle format
};

// Vectoring mode: (magnitude, atan2(y, x)) for a Q1.15 complex input,
// full range (all four quadrants, including x == 0). Backs
// bootstrap_detector's CFO/metric and frame_sync's correlation
// normalization (Phase 9.0b).
CordicVectorResult cordic_vector(int16_t x_q15, int16_t y_q15);

}  // namespace dsp
}  // namespace atsc3
