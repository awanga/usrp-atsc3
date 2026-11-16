// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// bootstrap_decoder.cc — A/321 bootstrap correlation and signaling decode
//
// Reference: ATSC A/321:2026 Sections 5.2-5.4, 6.1, Annex A

#include "bootstrap_decoder.h"

#include <cmath>
#include <stdexcept>

namespace atsc3 {
namespace sync {

namespace {

constexpr size_t kN = kBootstrapFftSize;

#ifdef ATSC3_FIXED_POINT
int16_t neg_sat(int16_t v) {
    return v == INT16_MIN ? INT16_MAX : static_cast<int16_t>(-v);
}
sample_t conj_sat(sample_t v) {
    return sample_t(v.real(), neg_sat(v.imag()));
}
int16_t round_q15(int32_t v) {
    int32_t r = (v + (1 << 14)) >> 15;
    return static_cast<int16_t>(r > INT16_MAX ? INT16_MAX : (r < INT16_MIN ? INT16_MIN : r));
}
#else
sample_t conj_sat(sample_t v) {
    return std::conj(v);
}
#endif

}  // namespace

BootstrapDecoder::BootstrapDecoder(const BootstrapDecoderConfig& config)
    : config_(config),
      reference_(kBootstrapSymbols, std::vector<sample_t>(kN)),
      work_(kN),
      bins_(kN),
      corr_(kN) {
    if (config_.max_integer_cfo_bins < 0 || config_.min_peak_to_mean <= 0.0) {
        throw std::invalid_argument("BootstrapDecoder: invalid configuration");
    }
    ofdm::FftConfig fc;
    fc.size = ofdm::FftSize::k2K;
    fc.direction = ofdm::FftDirection::kInverse;
    fc.normalize_inverse = true;
    ifft_ = ofdm::FftEngine::create(fc);

    std::vector<std::complex<float>> s(kN);
    for (size_t n = 0; n < kBootstrapSymbols; n++) {
        bootstrap_reference(0, 0, n, s.data());
        for (size_t k = 0; k < kN; k++) {
            std::complex<float> c = std::conj(s[k]);
#ifdef ATSC3_FIXED_POINT
            reference_[n][k] = sample_t(float_to_q15(c.real()), float_to_q15(c.imag()));
#else
            reference_[n][k] = c;
#endif
        }
    }
}

BootstrapDecoder::~BootstrapDecoder() = default;

void BootstrapDecoder::transform(const sample_t* window) {
    for (size_t t = 0; t < kN; t++) {
        work_[t] = conj_sat(window[t]);
    }
    ifft_->process(work_.data(), bins_.data());
    for (size_t k = 0; k < kN; k++) {
        bins_[k] = conj_sat(bins_[k]);
    }
}

BootstrapDecoder::Peak BootstrapDecoder::correlate(size_t n, int d) {
    const std::vector<sample_t>& ref = reference_[n];
    for (size_t k = 0; k < kN; k++) {
        size_t src = static_cast<size_t>(static_cast<long>(k) + d + static_cast<long>(kN)) % kN;
        const sample_t y = bins_[src];
        const sample_t r = ref[k];
#ifdef ATSC3_FIXED_POINT
        int32_t re = int32_t{y.real()} * r.real() - int32_t{y.imag()} * r.imag();
        int32_t im = int32_t{y.real()} * r.imag() + int32_t{y.imag()} * r.real();
        work_[k] = sample_t(round_q15(re), round_q15(im));
#else
        work_[k] = y * r;
#endif
    }
    ifft_->process(work_.data(), corr_.data());

    Peak p;
    for (size_t t = 0; t < kN; t++) {
#ifdef ATSC3_FIXED_POINT
        Power pw =
            int64_t{corr_[t].real()} * corr_[t].real() + int64_t{corr_[t].imag()} * corr_[t].imag();
#else
        Power pw = std::norm(corr_[t]);
#endif
        p.total += pw;
        if (pw > p.power) {
            p.power = pw;
            p.index = t;
            p.value = corr_[t];
        }
    }
    return p;
}

bool BootstrapDecoder::peak_stands_out(const Peak& p) const {
#ifdef ATSC3_FIXED_POINT
    // Threshold in Q4 so the comparison stays in integers.
    int64_t k16 = static_cast<int64_t>(std::lround(config_.min_peak_to_mean * 16.0));
    return p.total > 0 && p.power * static_cast<int64_t>(kN) * 16 >= k16 * p.total;
#else
    return p.total > 0.0 && p.power * static_cast<double>(kN) >= config_.min_peak_to_mean * p.total;
#endif
}

bool BootstrapDecoder::decode(const sample_t* in, BootstrapDecodeResult* out) {
    *out = BootstrapDecodeResult{};
    Peak peaks[kBootstrapSymbols];

    transform(in + kBootstrapCabOffsetA - kGuard);
    int best_d = 0;
    // Search outward from 0 so a tie keeps the smallest offset.
    for (int step = 0; step <= 2 * config_.max_integer_cfo_bins; step++) {
        int d = (step % 2 == 0) ? step / 2 : -(step + 1) / 2;
        Peak p = correlate(0, d);
        if (step == 0 || p.power > peaks[0].power) {
            peaks[0] = p;
            best_d = d;
        }
    }
    out->integer_cfo_bins = best_d;

    bool correlated = peak_stands_out(peaks[0]);
    for (size_t n = 1; n < kBootstrapSymbols; n++) {
        transform(in + n * kBootstrapSymbolLength + kBootstrapBcaOffsetA - kGuard);
        peaks[n] = correlate(n, best_d);
        correlated = correlated && peak_stands_out(peaks[n]);
    }
    out->correlated = correlated;

    // p_n = (guard + e - M_n) mod N for a symbol-0 start e and absolute
    // shift M_n, so M_n - M_{n-1} = p_{n-1} - p_n.
    int p0 = static_cast<int>(peaks[0].index);
    out->timing_offset = (p0 >= static_cast<int>(kN / 2) ? p0 - static_cast<int>(kN) : p0) -
                         static_cast<int>(kGuard);
    for (size_t n = 1; n < kBootstrapSymbols; n++) {
        unsigned rel = static_cast<unsigned>(peaks[n - 1].index + kN - peaks[n].index) % kN;
        out->relative_shift[n - 1] = static_cast<uint16_t>(rel);
        out->fields[n - 1] =
            static_cast<uint8_t>(bootstrap_gray_demap(rel, kBootstrapSignalingBits));
    }

    // Consecutive BCA windows are 3072 samples apart, so an integer offset
    // of d bins turns the second peak by 3072 d / 2048 turns, i.e. (-1)^d.
    // Symbol 1 is never the last (N_S >= 4), so 0 vs 1 is not compared.
    bool termination_ok = true;
    for (size_t n = 2; n < kBootstrapSymbols; n++) {
        const sample_t a = peaks[n].value;
        const sample_t b = peaks[n - 1].value;
#ifdef ATSC3_FIXED_POINT
        int64_t dot = int64_t{a.real()} * b.real() + int64_t{a.imag()} * b.imag();
#else
        double dot =
            static_cast<double>(a.real()) * b.real() + static_cast<double>(a.imag()) * b.imag();
#endif
        bool inverted = (best_d % 2 == 0) ? dot < 0 : dot > 0;
        termination_ok = termination_ok && inverted == (n == kBootstrapSymbols - 1);
    }
    out->termination_ok = termination_ok;
    out->fields_ok = parse_bootstrap_signaling(out->fields, &out->signaling);
    out->valid = out->correlated && out->termination_ok && out->fields_ok;
    return out->valid;
}

}  // namespace sync
}  // namespace atsc3
