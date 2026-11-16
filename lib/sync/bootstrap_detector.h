// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// Bootstrap Detector — ATSC 3.0 frame detection and fractional CFO
//
// Finds the first bootstrap symbol (A/321 Section 5.4.1, CAB structure)
// in a 6.144 MS/s stream from its two internal repetitions, so it needs no
// knowledge of the sequence and is insensitive to frequency offset:
//   - part C (520 samples) is a copy of the end of part A, 2048 samples
//     later: correlator P_C = sum x[u] conj(x[u - 2048]) over 520 samples;
//   - part B (504 samples) is a copy of the end of part A shifted up by
//     one subcarrier (3 kHz), 504 samples later: correlator
//     P_B = sum x[u] conj(x[u - 504]) exp(-j 2 pi u / 2048) over 504
//     samples (the free-running phase of the shift only rotates P_B).
// P_C is delayed by 504 samples so both windows end together at the last
// sample of symbol 0. Each is normalized by the energy of both its
// halves, R = sum (|x[u]|^2 + |x[u - D]|^2) / 2, so m = |P|^2 / R^2 is in
// [0, 1] (Cauchy-Schwarz). The detection metric is M = m_C x m_B, near
// zero on noise (each m ~ 1 / window length). Below a minimum correlator
// energy (near-silence) M is forced to 0.
//
// The BCA symbols that follow also have both repetitions (C/A at lag
// 2048, and C following B at lag 504 with the same +3 kHz relation,
// A/321 5.4.2), but their lag-504 window ends 504 samples away from where
// it would line up with their C/A window, so M only reaches about 1/16 of
// the symbol-0 peak there, at any SNR.
//
// Detection: arm when M exceeds threshold, track the peak, report on the
// falling edge below 0.8 x peak, then ignore the rest of the bootstrap
// (kHoldoff samples from the start of symbol 0). A bootstrap whose
// symbol 0 is missed can still trigger on its BCA symbols;
// BootstrapDecoder rejects those, as they do not correlate with symbol 0's
// sequence.
//
// The reported sample index is the start of symbol 0 (peak
// sample - 3071); the CFO is the phase of P_C at the peak, unambiguous to
// +-1.5 kHz (half the subcarrier spacing); BootstrapDecoder resolves the
// integer part and refines timing.
//
// With the default threshold the detector needs about -2 dB SNR; the
// metric scales as (SNR / (1 + SNR))^4.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=cf32 (ci16 fixed-point), TVALID, TREADY (6.144 MS/s IQ)
//   Output: Detection event with sample index and CFO estimate
//
// Reference: ATSC A/321:2026 Sections 5.1, 5.4

#include "sync/bootstrap_signal.h"
#include "types.h"

#include <complex>
#include <cstddef>
#include <vector>

namespace atsc3 {
namespace sync {

struct BootstrapDetection {
    bool detected = false;

    // Index (since reset) of the first sample of bootstrap symbol 0
    size_t sample_index = 0;

    // Fractional CFO estimate in Hz, within +-1.5 kHz
    double cfo_hz = 0.0;

    // Peak detection metric M = m_C x m_B (0-1)
    double metric = 0.0;

    // SNR estimate from m_C at the peak (sqrt(m_C) = S / (S + N)),
    // clamped to [-10, 30] dB
    double snr_db = 0.0;
};

struct BootstrapConfig {
    // Detection threshold on M (0-1)
    double threshold = 0.02;
};

class BootstrapDetector {
public:
    static constexpr size_t kLagC = kBootstrapPartA;  // 2048
    static constexpr size_t kLagB = kBootstrapPartB;  // 504
    // History needed by the delayed C window: 504 + 520 + 2048.
    static constexpr size_t kHistory = kLagB + kBootstrapPartC + kLagC;
    // One symbol past the end of a version 0.0 bootstrap, covering the
    // BCA windows' trailing edges.
    static constexpr size_t kHoldoff = (kBootstrapSymbols + 1) * kBootstrapSymbolLength;

    explicit BootstrapDetector(const BootstrapConfig& config = BootstrapConfig());
    ~BootstrapDetector();

    // Process one sample. detected=false unless a detection completes here.
    BootstrapDetection process(sample_t sample);

    // Process a buffer; returns the last detection in it.
    BootstrapDetection process(const sample_t* samples, size_t n);

    void reset();

    const BootstrapConfig& get_config() const {
        return config_;
    }
    void set_config(const BootstrapConfig& config);

    size_t get_sample_count() const {
        return sample_count_;
    }

    // Metric M and P_C-based CFO for the most recent sample, whatever the
    // detection state (monitoring, equivalence tests).
    double get_current_metric() const;
    double get_current_cfo_hz() const;

private:
    struct Terms;
    Terms terms_c(size_t u) const;
    Terms terms_b(size_t u) const;
    sample_t at(size_t u) const;  // x[u], u within the last kHistory + 1 samples
    BootstrapDetection check_detection();

    BootstrapConfig config_;
    std::vector<sample_t> history_;  // circular, kHistory + 1 samples
    size_t sample_count_ = 0;

#ifdef ATSC3_FIXED_POINT
    std::vector<sample_t> nco_;  // exp(-j 2 pi i / 2048), Q1.15
    // Raw Q1.15 x Q1.15 product sums (B terms rescaled by the Q1.15 NCO).
    int64_t pc_re_ = 0, pc_im_ = 0, rc_ = 0;
    int64_t pb_re_ = 0, pb_im_ = 0, rb_ = 0;
    int32_t metric_ = 0;             // Q1.15
    int32_t mc_ = 0;                 // Q1.15
    int16_t nc_re_ = 0, nc_im_ = 0;  // P_C / R_C, Q1.15
    int32_t peak_metric_ = 0;
    int32_t peak_mc_ = 0;
    int16_t peak_nc_re_ = 0, peak_nc_im_ = 0;
#else
    std::vector<std::complex<double>> nco_;
    std::complex<double> pc_{}, pb_{};
    double rc_ = 0.0, rb_ = 0.0;
    double metric_ = 0.0;
    double mc_ = 0.0;
    std::complex<double> nc_{};
    double peak_metric_ = 0.0;
    double peak_mc_ = 0.0;
    std::complex<double> peak_nc_{};
#endif
    bool in_detection_ = false;
    size_t peak_sample_ = 0;
    size_t holdoff_end_ = 0;  // no arming before this sample index
};

}  // namespace sync
}  // namespace atsc3
