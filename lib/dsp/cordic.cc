#include "cordic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace atsc3 {
namespace dsp {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// atan(2^-i) for i in [0, kCordicIterations), in the Q1.15 angle format
// (value = truncate(atan(2^-i) / pi * 32768)). Precomputed offline rather
// than calling std::atan() at runtime -- these are constants of the
// algorithm, not data.
constexpr int32_t kAtanTableQ15[kCordicIterations] = {
    8192, 4836, 2555, 1297, 651, 325, 162, 81, 40, 20, 10, 5, 2, 1,
};

int16_t saturate_i16(int32_t v) {
    if (v > 32767) {
        return 32767;
    }
    if (v < -32767) {
        return -32767;
    }
    return static_cast<int16_t>(v);
}

// Negate a Q1.15 value without UB/wraparound on the one unrepresentable
// input (-32768 has no positive counterpart in [-32767, 32767]).
int16_t saturating_negate(int16_t v) {
    if (v == -32768) {
        return 32767;
    }
    return static_cast<int16_t>(-v);
}

}  // namespace

int16_t float_to_q15_angle(float radians) {
    float turns = radians / kPi;
    float scaled = turns * 32768.0f;
    if (scaled > 32767.0f) {
        return 32767;
    }
    // Unlike float_to_q15() for Q1.15 samples, -32768 is a legitimate,
    // intended output here: it's the -pi boundary, and +pi itself isn't
    // representable (same asymmetry as +1.0 in the sample format), so
    // -32768 is the correct representation for the (identical) +pi/-pi
    // angle, not a value to clamp away.
    if (scaled < -32768.0f) {
        return -32768;
    }
    return static_cast<int16_t>(scaled);
}

float q15_angle_to_float(int16_t angle) {
    return (static_cast<float>(angle) / 32768.0f) * kPi;
}

int16_t cordic_add_half_turn(int16_t angle) {
    // Well-defined unsigned wraparound implements "+pi mod 2*pi" in one
    // step -- see the header comment for why this is exact in this angle
    // format. Equivalent to flipping bit 15.
    uint16_t bits = static_cast<uint16_t>(angle);
    bits = static_cast<uint16_t>(bits + 0x8000u);
    return static_cast<int16_t>(bits);
}

CordicRotationResult cordic_rotate(int16_t theta_q15_angle) {
    // Basic CORDIC rotation converges only for angles within roughly
    // +-99.9 degrees of the positive x-axis. Reduce anything outside
    // +-pi/2 (16384 in this Q1.15 angle format) by one half turn, and
    // correct the sign of both outputs afterward.
    bool flip = false;
    int32_t theta = theta_q15_angle;
    if (theta_q15_angle > 16384 || theta_q15_angle < -16384) {
        theta = cordic_add_half_turn(theta_q15_angle);
        flip = true;
    }

    int32_t x = kCordicInvGainQ15;  // pre-scaled so output is unity-gain
    int32_t y = 0;
    int32_t z = theta;

    for (int i = 0; i < kCordicIterations; ++i) {
        int32_t d = (z >= 0) ? 1 : -1;
        int32_t x_next = x - d * (y >> i);
        int32_t y_next = y + d * (x >> i);
        int32_t z_next = z - d * kAtanTableQ15[i];
        x = x_next;
        y = y_next;
        z = z_next;
    }

    int16_t cos_theta = saturate_i16(x);
    int16_t sin_theta = saturate_i16(y);
    if (flip) {
        cos_theta = saturating_negate(cos_theta);
        sin_theta = saturating_negate(sin_theta);
    }
    return CordicRotationResult{cos_theta, sin_theta};
}

CordicVectorResult cordic_vector(int16_t x_q15, int16_t y_q15) {
    // (0, 0) is degenerate: magnitude is unambiguously 0, but the angle
    // is mathematically undefined (matches std::atan2(0, 0), which is
    // conventionally defined as 0 rather than left undefined). Without
    // this, the main loop's y>=0 tie-break (see below) drifts z away
    // from 0 every iteration even though there is no real rotation to
    // measure, since x and y never move when both start at 0.
    if (x_q15 == 0 && y_q15 == 0) {
        return CordicVectorResult{0, 0};
    }

    // Vectoring mode converges driving y to 0 only for vectors starting in
    // the right half-plane (x0 >= 0). Reflect through the origin when
    // x < 0 and correct the resulting angle by one half turn afterward --
    // magnitude is unaffected by the reflection.
    bool flip = (x_q15 < 0);
    int32_t x = flip ? -static_cast<int32_t>(x_q15) : static_cast<int32_t>(x_q15);
    int32_t y = flip ? -static_cast<int32_t>(y_q15) : static_cast<int32_t>(y_q15);
    int32_t z = 0;

    for (int i = 0; i < kCordicIterations; ++i) {
        int32_t d = (y >= 0) ? -1 : 1;
        int32_t x_next = x - d * (y >> i);
        int32_t y_next = y + d * (x >> i);
        int32_t z_next = z - d * kAtanTableQ15[i];
        x = x_next;
        y = y_next;
        z = z_next;
    }

    // x is gain-scaled by K (~1.6468); rescale to a true magnitude.
    // Widen to int64_t for the multiply: x can be ~76000 in the worst
    // case (32767 * K), times a Q1.15 constant (~2e4), comfortably inside
    // int64_t but not int32_t.
    int64_t magnitude_wide = (static_cast<int64_t>(x) * kCordicInvGainQ15) >> 15;

    int16_t angle = saturate_i16(z);
    if (flip) {
        angle = cordic_add_half_turn(angle);
    }

    return CordicVectorResult{static_cast<int32_t>(magnitude_wide), angle};
}

}  // namespace dsp
}  // namespace atsc3
