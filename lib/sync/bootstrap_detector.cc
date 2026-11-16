// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// bootstrap_detector.cc — A/321 CAB-structure bootstrap detection
//
// Reference: ATSC A/321:2026 Sections 5.1, 5.4

#include "bootstrap_detector.h"

#include "dsp/cordic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace atsc3 {
namespace sync {

namespace {

constexpr long kLagC = static_cast<long>(BootstrapDetector::kLagC);
constexpr long kLagB = static_cast<long>(BootstrapDetector::kLagB);
constexpr long kWinC = static_cast<long>(kBootstrapPartC);
constexpr long kWinB = static_cast<long>(kBootstrapPartB);
// The last sample of symbol 0 is where both windows line up.
constexpr size_t kPeakToStart = kBootstrapSymbolLength - 1;

// Near-silence floor on each R (sum of |x|^2 + |x_d|^2 over ~512 samples,
// raw Q1.15 x Q1.15 units): 2^16 is an rms amplitude of about 8 LSB
// (~-72 dBFS), far below any real input after AGC. Without it, exact
// silence leaves 0/0 ratios.
constexpr int64_t kMinCorrelatorEnergyQ30 = int64_t{1} << 16;

constexpr double kSnrFloorDb = -10.0;
constexpr double kSnrCeilDb = 30.0;

// On a repeated structure P -> S and R -> S + N per sample, so
// m = |P|^2 / R^2 -> rho^2 with rho = S / (S + N) and SNR = rho / (1 - rho).
double snr_db_from_m(double m) {
    double rho = std::sqrt(std::max(0.0, m));
    if (rho <= 0.0) {
        return kSnrFloorDb;
    }
    if (rho >= 1.0) {
        return kSnrCeilDb;
    }
    return std::min(kSnrCeilDb, std::max(kSnrFloorDb, 10.0 * std::log10(rho / (1.0 - rho))));
}

#ifdef ATSC3_FIXED_POINT
int16_t saturate_i16(int64_t v) {
    return static_cast<int16_t>(std::min<int64_t>(32767, std::max<int64_t>(-32767, v)));
}
// angle (Q1.15, pi = 32768) x Fs / (2 pi x 2048) = angle x Fs >> 27.
double cfo_from_angle(int16_t angle) {
    int64_t fs = static_cast<int64_t>(kBootstrapSampleRateHz);
    return static_cast<double>((static_cast<int64_t>(angle) * fs) >> 27);
}
#else
constexpr double kMinCorrelatorEnergy = static_cast<double>(kMinCorrelatorEnergyQ30) / 1073741824.0;
double cfo_from_phase(double phase) {
    return phase * kBootstrapSampleRateHz / (2.0 * M_PI * static_cast<double>(kLagC));
}
#endif

}  // namespace

#ifdef ATSC3_FIXED_POINT
struct BootstrapDetector::Terms {
    int64_t re = 0;
    int64_t im = 0;
    int64_t energy = 0;
};
#else
struct BootstrapDetector::Terms {
    std::complex<double> corr{};
    double energy = 0.0;
};
#endif

BootstrapDetector::BootstrapDetector(const BootstrapConfig& config)
    : config_(config), history_(kHistory + 1), nco_(kBootstrapFftSize) {
    for (size_t i = 0; i < kBootstrapFftSize; i++) {
        double a = -2.0 * M_PI * static_cast<double>(i) / static_cast<double>(kBootstrapFftSize);
#ifdef ATSC3_FIXED_POINT
        nco_[i] = sample_t(float_to_q15(static_cast<float>(std::cos(a))),
                           float_to_q15(static_cast<float>(std::sin(a))));
#else
        nco_[i] = std::complex<double>(std::cos(a), std::sin(a));
#endif
    }
    reset();
}

BootstrapDetector::~BootstrapDetector() = default;

void BootstrapDetector::reset() {
    std::fill(history_.begin(), history_.end(), sample_t(0, 0));
    sample_count_ = 0;
#ifdef ATSC3_FIXED_POINT
    pc_re_ = pc_im_ = rc_ = 0;
    pb_re_ = pb_im_ = rb_ = 0;
    metric_ = mc_ = 0;
    nc_re_ = nc_im_ = 0;
    peak_metric_ = peak_mc_ = 0;
    peak_nc_re_ = peak_nc_im_ = 0;
#else
    pc_ = pb_ = 0.0;
    rc_ = rb_ = 0.0;
    metric_ = mc_ = 0.0;
    nc_ = peak_nc_ = 0.0;
    peak_metric_ = peak_mc_ = 0.0;
#endif
    in_detection_ = false;
    peak_sample_ = 0;
    holdoff_end_ = 0;
}

void BootstrapDetector::set_config(const BootstrapConfig& config) {
    config_ = config;
}

sample_t BootstrapDetector::at(size_t u) const {
    return history_[u % history_.size()];
}

// x[v - 504] conj(x[v - 2552]): part C against the end of part A, delayed
// so the window ends with part B's.
BootstrapDetector::Terms BootstrapDetector::terms_c(size_t v) const {
    Terms t;
    if (v < static_cast<size_t>(kLagB + kLagC)) {
        return t;
    }
    sample_t a = at(v - kLagB);
    sample_t b = at(v - kLagB - kLagC);
#ifdef ATSC3_FIXED_POINT
    t.re = int64_t{a.real()} * b.real() + int64_t{a.imag()} * b.imag();
    t.im = int64_t{a.imag()} * b.real() - int64_t{a.real()} * b.imag();
    t.energy = int64_t{a.real()} * a.real() + int64_t{a.imag()} * a.imag() +
               int64_t{b.real()} * b.real() + int64_t{b.imag()} * b.imag();
#else
    std::complex<double> ad(a.real(), a.imag()), bd(b.real(), b.imag());
    t.corr = ad * std::conj(bd);
    t.energy = std::norm(ad) + std::norm(bd);
#endif
    return t;
}

// x[v] conj(x[v - 504]) exp(-j 2 pi v / 2048): part B against the end of
// part A, with B's one-subcarrier shift removed.
BootstrapDetector::Terms BootstrapDetector::terms_b(size_t v) const {
    Terms t;
    if (v < static_cast<size_t>(kLagB)) {
        return t;
    }
    sample_t a = at(v);
    sample_t b = at(v - kLagB);
    const auto& w = nco_[v % kBootstrapFftSize];
#ifdef ATSC3_FIXED_POINT
    int64_t re = int64_t{a.real()} * b.real() + int64_t{a.imag()} * b.imag();
    int64_t im = int64_t{a.imag()} * b.real() - int64_t{a.real()} * b.imag();
    t.re = (re * w.real() - im * w.imag()) >> 15;
    t.im = (re * w.imag() + im * w.real()) >> 15;
    t.energy = int64_t{a.real()} * a.real() + int64_t{a.imag()} * a.imag() +
               int64_t{b.real()} * b.real() + int64_t{b.imag()} * b.imag();
#else
    std::complex<double> ad(a.real(), a.imag()), bd(b.real(), b.imag());
    t.corr = ad * std::conj(bd) * w;
    t.energy = std::norm(ad) + std::norm(bd);
#endif
    return t;
}

BootstrapDetection BootstrapDetector::process(sample_t sample) {
    const size_t u = sample_count_;
    history_[u % history_.size()] = sample;
    sample_count_++;

    // Sliding sums: add the newest term, drop the one that left the
    // window (recomputed from the history, so the integer sums cancel
    // exactly).
    Terms c_in = terms_c(u);
    Terms b_in = terms_b(u);
    Terms c_out = u >= static_cast<size_t>(kWinC) ? terms_c(u - kWinC) : Terms{};
    Terms b_out = u >= static_cast<size_t>(kWinB) ? terms_b(u - kWinB) : Terms{};
#ifdef ATSC3_FIXED_POINT
    pc_re_ += c_in.re - c_out.re;
    pc_im_ += c_in.im - c_out.im;
    rc_ += c_in.energy - c_out.energy;
    pb_re_ += b_in.re - b_out.re;
    pb_im_ += b_in.im - b_out.im;
    rb_ += b_in.energy - b_out.energy;
#else
    pc_ += c_in.corr - c_out.corr;
    rc_ += c_in.energy - c_out.energy;
    pb_ += b_in.corr - b_out.corr;
    rb_ += b_in.energy - b_out.energy;
#endif
    return check_detection();
}

BootstrapDetection BootstrapDetector::check_detection() {
    BootstrapDetection result;
    const size_t u = sample_count_ - 1;

#ifdef ATSC3_FIXED_POINT
    // 2P/R in Q1.15 (|P| <= R/2 with R summing both halves' energy).
    // |P| < 2^41, so 2P * 32768 < 2^57: no overflow.
    int16_t nb_re = 0, nb_im = 0;
    nc_re_ = nc_im_ = 0;
    if (rc_ >= kMinCorrelatorEnergyQ30 && rb_ >= kMinCorrelatorEnergyQ30) {
        nc_re_ = saturate_i16((2 * pc_re_ * 32768) / rc_);
        nc_im_ = saturate_i16((2 * pc_im_ * 32768) / rc_);
        nb_re = saturate_i16((2 * pb_re_ * 32768) / rb_);
        nb_im = saturate_i16((2 * pb_im_ * 32768) / rb_);
    }
    mc_ = (int32_t{nc_re_} * nc_re_ + int32_t{nc_im_} * nc_im_) >> 15;
    int32_t mb = (int32_t{nb_re} * nb_re + int32_t{nb_im} * nb_im) >> 15;
    metric_ = static_cast<int32_t>((int64_t{mc_} * mb) >> 15);
    const int32_t threshold = float_to_q15(static_cast<float>(config_.threshold));
#else
    double mb = 0.0;
    nc_ = 0.0;
    if (rc_ >= kMinCorrelatorEnergy && rb_ >= kMinCorrelatorEnergy) {
        nc_ = 2.0 * pc_ / rc_;
        mb = std::norm(2.0 * pb_ / rb_);
    }
    mc_ = std::norm(nc_);
    metric_ = mc_ * mb;
    const double threshold = config_.threshold;
#endif

    if (!in_detection_) {
        if (u < holdoff_end_ || !(metric_ > threshold)) {
            return result;
        }
        in_detection_ = true;
        peak_metric_ = 0;
    }
    if (metric_ > peak_metric_) {
        peak_metric_ = metric_;
        peak_mc_ = mc_;
        peak_sample_ = u;
#ifdef ATSC3_FIXED_POINT
        peak_nc_re_ = nc_re_;
        peak_nc_im_ = nc_im_;
#else
        peak_nc_ = nc_;
#endif
    }

#ifdef ATSC3_FIXED_POINT
    bool fell = metric_ < ((int64_t{peak_metric_} * float_to_q15(0.8f)) >> 15);
#else
    bool fell = metric_ < 0.8 * peak_metric_;
#endif
    if (!fell) {
        return result;
    }
    in_detection_ = false;
    if (peak_sample_ < kPeakToStart) {
        return result;  // too early to hold a whole symbol 0
    }
    result.detected = true;
    result.sample_index = peak_sample_ - kPeakToStart;
    holdoff_end_ = result.sample_index + kHoldoff;
#ifdef ATSC3_FIXED_POINT
    result.metric = static_cast<double>(peak_metric_) / 32768.0;
    result.cfo_hz = cfo_from_angle(dsp::cordic_vector(peak_nc_re_, peak_nc_im_).angle_q15);
    // log10 has no fixed-point counterpart here (see
    // hdl/docs/placeholder_status.md); computed in double at this output only.
    result.snr_db = snr_db_from_m(static_cast<double>(peak_mc_) / 32768.0);
#else
    result.metric = peak_metric_;
    result.cfo_hz = cfo_from_phase(std::arg(peak_nc_));
    result.snr_db = snr_db_from_m(peak_mc_);
#endif
    return result;
}

BootstrapDetection BootstrapDetector::process(const sample_t* samples, size_t n) {
    BootstrapDetection last;
    for (size_t i = 0; i < n; ++i) {
        BootstrapDetection det = process(samples[i]);
        if (det.detected) {
            last = det;
        }
    }
    return last;
}

double BootstrapDetector::get_current_metric() const {
#ifdef ATSC3_FIXED_POINT
    return static_cast<double>(metric_) / 32768.0;
#else
    return metric_;
#endif
}

double BootstrapDetector::get_current_cfo_hz() const {
#ifdef ATSC3_FIXED_POINT
    return cfo_from_angle(dsp::cordic_vector(nc_re_, nc_im_).angle_q15);
#else
    return cfo_from_phase(std::arg(nc_));
#endif
}

}  // namespace sync
}  // namespace atsc3
