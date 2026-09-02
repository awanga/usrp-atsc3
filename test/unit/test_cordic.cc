// test_cordic.cc — Unit tests for lib/dsp/cordic.h
//
// Acceptance bar (per the HDL port plan, Phase 9.0b): >=40 dB SNR against
// std::cos/sin/atan2/hypot across the full input range. This is the
// equivalence test that has to pass before any block is rewritten to use
// this CORDIC core.

#include "cordic.h"

#include <cmath>
#include <gtest/gtest.h>

namespace atsc3 {
namespace dsp {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Local Q1.15 <-> float helpers: cordic.cc has no ATSC3_FIXED_POINT guard
// and this test must run in both build configs, so it can't rely on
// types.h's q15_to_float()/float_to_q15(), which are guarded.
double q15_to_double(int32_t v) {
    return static_cast<double>(v) / 32768.0;
}

// Wrap a radian difference into (-pi, pi] before treating it as an error
// sample -- angle +pi and -pi are the same physical angle, and the one
// boundary point where cordic_vector lands on the -32768 (i.e. -pi)
// representation while std::atan2 returns +pi must not read as a ~2*pi
// error spike.
double wrap_angle_diff(double diff) {
    while (diff > kPi)
        diff -= 2.0 * kPi;
    while (diff < -kPi)
        diff += 2.0 * kPi;
    return diff;
}

double snr_db(double signal_power, double error_power) {
    if (error_power <= 0.0) {
        return 300.0;  // effectively exact; avoid div-by-zero / log(0)
    }
    return 10.0 * std::log10(signal_power / error_power);
}

//==============================================================================
// Angle format helpers
//==============================================================================

TEST(CordicTest, AngleRoundTripKnownValues) {
    EXPECT_EQ(float_to_q15_angle(0.0f), 0);
    EXPECT_NEAR(q15_angle_to_float(0), 0.0, 1e-6);

    // +pi is not representable (matches float_to_q15's +1.0 caveat);
    // -pi is.
    EXPECT_EQ(float_to_q15_angle(static_cast<float>(kPi)), 32767);
    EXPECT_EQ(float_to_q15_angle(static_cast<float>(-kPi)), -32768);

    EXPECT_NEAR(q15_angle_to_float(16384), kPi / 2.0, 1e-3);
    EXPECT_NEAR(q15_angle_to_float(-16384), -kPi / 2.0, 1e-3);
}

TEST(CordicTest, AddHalfTurnIsSelfInverse) {
    for (int32_t v = -32768; v <= 32767; v += 997) {
        int16_t a = static_cast<int16_t>(v);
        int16_t once = cordic_add_half_turn(a);
        int16_t twice = cordic_add_half_turn(once);
        EXPECT_EQ(twice, a) << "half-turn applied twice must return to start, a=" << a;
    }
}

TEST(CordicTest, AddHalfTurnKnownValues) {
    EXPECT_EQ(cordic_add_half_turn(0), -32768);
    EXPECT_EQ(cordic_add_half_turn(-32768), 0);
    EXPECT_EQ(cordic_add_half_turn(16384), -16384);
    EXPECT_EQ(cordic_add_half_turn(-16384), 16384);
}

//==============================================================================
// Rotation mode
//==============================================================================

TEST(CordicTest, RotateKnownAngles) {
    // theta = 0: cos = 1 (saturates to 32767, exact 1.0 unrepresentable),
    // sin = 0 exactly.
    auto r0 = cordic_rotate(0);
    EXPECT_GE(r0.cos_theta, 32700);
    EXPECT_NEAR(r0.sin_theta, 0, 20);

    // theta = pi/2: cos ~ 0, sin ~ 1
    auto r90 = cordic_rotate(16384);
    EXPECT_NEAR(r90.cos_theta, 0, 40);
    EXPECT_GE(r90.sin_theta, 32700);

    // theta = -pi/2: cos ~ 0, sin ~ -1
    auto rm90 = cordic_rotate(-16384);
    EXPECT_NEAR(rm90.cos_theta, 0, 40);
    EXPECT_LE(rm90.sin_theta, -32700);

    // theta = -pi (the representable boundary): cos ~ -1, sin ~ 0
    auto r180 = cordic_rotate(-32768);
    EXPECT_LE(r180.cos_theta, -32700);
    EXPECT_NEAR(r180.sin_theta, 0, 40);
}

TEST(CordicTest, RotateAccuracyAcrossFullRange) {
    constexpr int kSamples = 2000;
    double signal_power = 0.0;
    double error_power = 0.0;

    for (int i = 0; i < kSamples; ++i) {
        double theta = -kPi + (2.0 * kPi * i) / kSamples;
        int16_t theta_q15 = float_to_q15_angle(static_cast<float>(theta));

        auto result = cordic_rotate(theta_q15);
        double cos_fixed = q15_to_double(result.cos_theta);
        double sin_fixed = q15_to_double(result.sin_theta);

        double cos_ref = std::cos(theta);
        double sin_ref = std::sin(theta);

        signal_power += cos_ref * cos_ref + sin_ref * sin_ref;
        double dc = cos_fixed - cos_ref;
        double ds = sin_fixed - sin_ref;
        error_power += dc * dc + ds * ds;
    }

    double snr = snr_db(signal_power, error_power);
    EXPECT_GE(snr, 40.0) << "CORDIC rotation-mode SNR vs. std::cos/sin: " << snr << " dB";
}

TEST(CordicTest, RotateContinuousAcrossQuadrantBoundary) {
    // The +-pi/2 quadrant-reduction boundary is exactly where a sign or
    // off-by-one bug in the flip logic would show up as a discontinuity.
    for (int16_t theta : {int16_t(16383), int16_t(16384), int16_t(16385), int16_t(-16383),
                          int16_t(-16384), int16_t(-16385)}) {
        auto result = cordic_rotate(theta);
        double theta_f = q15_angle_to_float(theta);
        EXPECT_NEAR(q15_to_double(result.cos_theta), std::cos(theta_f), 0.01)
            << "theta_q15=" << theta;
        EXPECT_NEAR(q15_to_double(result.sin_theta), std::sin(theta_f), 0.01)
            << "theta_q15=" << theta;
    }
}

//==============================================================================
// Vectoring mode
//==============================================================================

TEST(CordicTest, VectorKnownPoints) {
    // (1, 0): magnitude ~1, angle 0
    auto v0 = cordic_vector(32767, 0);
    EXPECT_NEAR(v0.magnitude, 32767, 30);
    EXPECT_NEAR(v0.angle_q15, 0, 20);

    // (0, 1): magnitude ~1, angle pi/2
    auto v90 = cordic_vector(0, 32767);
    EXPECT_NEAR(v90.magnitude, 32767, 30);
    EXPECT_NEAR(v90.angle_q15, 16384, 40);

    // (0, 0): both zero
    auto vzero = cordic_vector(0, 0);
    EXPECT_EQ(vzero.magnitude, 0);
    EXPECT_EQ(vzero.angle_q15, 0);

    // (-1, 0): magnitude ~1, angle wraps to the -pi representable boundary
    // (see wrap_angle_diff's comment -- +pi and -pi are the same angle,
    // and +pi isn't representable in this format).
    auto vneg = cordic_vector(-32767, 0);
    EXPECT_NEAR(vneg.magnitude, 32767, 30);
    EXPECT_NEAR(std::abs(vneg.angle_q15), 32768, 10);
}

TEST(CordicTest, VectorAccuracyAcrossFullRange) {
    constexpr int kMagSteps = 20;
    constexpr int kAngleSteps = 200;
    double mag_signal_power = 0.0;
    double mag_error_power = 0.0;
    double angle_signal_power = 0.0;
    double angle_error_power = 0.0;

    for (int m = 1; m <= kMagSteps; ++m) {
        double mag = (static_cast<double>(m) / kMagSteps) * 0.999;
        for (int a = 0; a < kAngleSteps; ++a) {
            double theta = -kPi + (2.0 * kPi * a) / kAngleSteps;
            double xf = mag * std::cos(theta);
            double yf = mag * std::sin(theta);
            int16_t x_q15 = static_cast<int16_t>(xf * 32768.0);
            int16_t y_q15 = static_cast<int16_t>(yf * 32768.0);

            auto result = cordic_vector(x_q15, y_q15);
            double mag_fixed = q15_to_double(result.magnitude);
            double angle_fixed = q15_angle_to_float(result.angle_q15);

            double mag_ref = std::hypot(q15_to_double(x_q15), q15_to_double(y_q15));
            double angle_ref = std::atan2(q15_to_double(y_q15), q15_to_double(x_q15));

            mag_signal_power += mag_ref * mag_ref;
            double dm = mag_fixed - mag_ref;
            mag_error_power += dm * dm;

            angle_signal_power += angle_ref * angle_ref;
            double da = wrap_angle_diff(angle_fixed - angle_ref);
            angle_error_power += da * da;
        }
    }

    double mag_snr = snr_db(mag_signal_power, mag_error_power);
    double angle_snr = snr_db(angle_signal_power, angle_error_power);
    EXPECT_GE(mag_snr, 40.0) << "CORDIC vectoring-mode magnitude SNR vs. std::hypot: " << mag_snr
                             << " dB";
    EXPECT_GE(angle_snr, 40.0) << "CORDIC vectoring-mode angle SNR vs. std::atan2: " << angle_snr
                               << " dB";
}

TEST(CordicTest, VectorContinuousAcrossQuadrantBoundary) {
    // x crossing 0 is where the vectoring-mode reflection logic engages.
    for (int16_t x : {int16_t(1), int16_t(0), int16_t(-1)}) {
        for (int16_t y : {int16_t(10000), int16_t(-10000)}) {
            auto result = cordic_vector(x, y);
            double mag_ref = std::hypot(q15_to_double(x), q15_to_double(y));
            double angle_ref = std::atan2(q15_to_double(y), q15_to_double(x));
            EXPECT_NEAR(q15_to_double(result.magnitude), mag_ref, 0.01) << "x=" << x << " y=" << y;
            EXPECT_NEAR(wrap_angle_diff(q15_angle_to_float(result.angle_q15) - angle_ref), 0.0,
                        0.05)
                << "x=" << x << " y=" << y;
        }
    }
}

}  // namespace
}  // namespace dsp
}  // namespace atsc3
