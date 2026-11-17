// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// preamble_demod.cc — preamble symbol equalization and de-interleaving
//
// Reference: ATSC A/322:2026 Sections 7.2.5, 7.3, 8.1, 8.3

#include "preamble_demod.h"

#include "config/atsc3_config.h"

#include <algorithm>
#include <stdexcept>

namespace atsc3 {
namespace framing {

namespace {

// Section 7.2.5.1: the first preamble symbol uses the minimum NoC.
constexpr unsigned kFirstSymbolCred = 4;

}  // namespace

void PreambleDemodulator::build(size_t fft_size, size_t dx, unsigned cred, SymbolPath* path) {
    if (!ofdm::preamble_layout(fft_size, dx, cred, &path->layout)) {
        throw std::invalid_argument("PreambleDemodulator: FFT size / Dx not in Table H.1.1");
    }
    const size_t noc = path->layout.noc;
    path->first_bin = config::first_carrier_bin(fft_size, noc);

    double pilot_db = 0.0;
    ofdm::preamble_pilot_power_db(fft_size, dx, &pilot_db);
    const sample_t::value_type inv_pilot = ofdm::pilot_inverse_amplitude(pilot_db);
    const sample_t::value_type inv_cp = ofdm::pilot_inverse_amplitude(ofdm::kContinualPilotPowerDb);
    const std::vector<uint8_t> r = ofdm::pilot_reference_sequence(noc);

    // Pilots and continual pilots, ascending k (the estimator interpolates
    // between neighbours in the order given).
    std::vector<std::pair<uint32_t, bool>> refs;  // (k, continual)
    for (uint32_t k : path->layout.pilots) {
        refs.emplace_back(k, false);
    }
    for (uint32_t k : path->layout.continual) {
        refs.emplace_back(k, true);
    }
    std::sort(refs.begin(), refs.end());
    path->pilots.clear();
    for (const auto& [k, continual] : refs) {
        ofdm::PilotSymbol p{};
        p.subcarrier_index = path->first_bin + k;
        sample_t::value_type a = continual ? inv_cp : inv_pilot;
        // X = 2 A (1/2 - r_k): +A for r_k = 0, -A for r_k = 1.
        p.inverse_reference =
            sample_t(static_cast<sample_t::value_type>(r[k] ? -a : a), sample_t::value_type{});
        p.type = continual ? ofdm::PilotType::CONTINUAL : ofdm::PilotType::SCATTERED;
        path->pilots.push_back(p);
    }

    channel::ChannelEstimatorConfig ce;
    ce.fft_size = fft_size;
    ce.first_active_carrier = path->first_bin;
    ce.num_active_carriers = noc;
    path->estimator.set_config(ce);

    channel::EqualizerConfig eq;
    eq.fft_size = fft_size;
    eq.first_active_carrier = path->first_bin;
    eq.num_active_carriers = noc;
    eq.mode = channel::EqualizationMode::ZERO_FORCING;
    eq.enable_phase_tracking = false;
    path->equalizer.set_config(eq);
}

PreambleDemodulator::PreambleDemodulator(const PreambleDemodConfig& config) : config_(config) {
    if (config_.later_cred > 4) {
        throw std::invalid_argument("PreambleDemodulator: Cred_coeff above 4");
    }
    build(config_.fft_size, config_.dx, kFirstSymbolCred, &first_);
    build(config_.fft_size, config_.dx, config_.later_cred, &later_);
    ofdm::FreqDeinterleaverConfig fd;
    fd.fft_size = config_.fft_size;
    deinterleaver_.set_config(fd);
    scratch_.resize(std::max(first_.layout.data.size(), later_.layout.data.size()));
}

void PreambleDemodulator::reset() {
    symbol_ = 0;
    deinterleaver_.reset();
    first_.estimator.reset();
    later_.estimator.reset();
}

size_t PreambleDemodulator::num_cells() const {
    return (symbol_ == 0 ? first_ : later_).layout.data.size();
}

size_t PreambleDemodulator::demodulate(const sample_t* bins, sample_t* cells) {
    SymbolPath& path = (symbol_ == 0) ? first_ : later_;
    for (ofdm::PilotSymbol& p : path.pilots) {
        p.received_value = bins[p.subcarrier_index];
    }
    const channel::ChannelEstimate est = path.estimator.estimate(path.pilots, symbol_);
    const std::vector<uint32_t>& data = path.layout.data;
    for (size_t i = 0; i < data.size(); ++i) {
        scratch_[i] =
            path.equalizer.equalize_sample(bins[path.first_bin + data[i]], est.h_estimate[data[i]]);
    }
    deinterleaver_.deinterleave(scratch_.data(), cells, data.size());
    ++symbol_;
    return data.size();
}

}  // namespace framing
}  // namespace atsc3
