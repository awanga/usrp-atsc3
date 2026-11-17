// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// channel_estimator_gen.cc — golden-vector CLI for hdl/rtl/channel/
//
// Links the real atsc3_lib (lib/channel/channel_estimator.h/.cc, built with
// ATSC3_FIXED_POINT=ON). One stdin stream drives the stateless divider and
// the stateful estimator:
//
//   D <a_re> <a_im> <b_re> <b_im>
//       -> <re> <im>                  safe_complex_divide(a, b)
//   C <fft_size> <num_active, 0 = default> <avg_enable 0/1> <alpha_q15>
//       configures the estimator (alpha = alpha_q15 / 32768, exact in float)
//   S <symbol_index> <n_pilots>       -- then n_pilots lines:
//       <carrier> <ref_re> <ref_im> <rx_re> <rx_im>
//       -> S <symbol_index> <num_active>
//          <re> <im>                  per active carrier, relative order
//
// Pilot carriers are absolute FFT bins, ascending, inside the active window,
// the same stream pilot_extractor produces.
//
// Usage: channel_estimator_gen < stimulus.txt > results.txt

#include "channel/channel_estimator.h"
#include "ofdm/pilot_extractor.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

int main() {
    using atsc3::sample_t;
    using atsc3::channel::ChannelEstimator;
    using atsc3::channel::ChannelEstimatorConfig;
    using atsc3::ofdm::PilotSymbol;

    std::unique_ptr<ChannelEstimator> estimator;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream in(line);
        char tag = 0;
        in >> tag;
        if (tag == 'D') {
            int v[4];
            if (!(in >> v[0] >> v[1] >> v[2] >> v[3])) {
                std::cerr << "channel_estimator_gen: bad divide line: " << line << '\n';
                return 1;
            }
            const sample_t q = atsc3::channel::safe_complex_divide(
                sample_t(static_cast<int16_t>(v[0]), static_cast<int16_t>(v[1])),
                sample_t(static_cast<int16_t>(v[2]), static_cast<int16_t>(v[3])));
            std::cout << q.real() << ' ' << q.imag() << '\n';
        } else if (tag == 'C') {
            ChannelEstimatorConfig config;
            int avg = 0;
            int alpha_q15 = 0;
            if (!(in >> config.fft_size >> config.num_active_carriers >> avg >> alpha_q15) ||
                alpha_q15 < 0 || alpha_q15 > 32768) {
                std::cerr << "channel_estimator_gen: bad config line: " << line << '\n';
                return 1;
            }
            config.enable_averaging = avg != 0;
            config.averaging_alpha = static_cast<float>(alpha_q15) / 32768.0f;
            estimator = std::make_unique<ChannelEstimator>(config);
        } else if (tag == 'S') {
            size_t symbol_index = 0;
            size_t n = 0;
            if (!estimator || !(in >> symbol_index >> n) || n == 0) {
                std::cerr << "channel_estimator_gen: bad symbol header: " << line << '\n';
                return 1;
            }
            std::vector<PilotSymbol> pilots;
            for (size_t i = 0; i < n; ++i) {
                size_t carrier = 0;
                int v[4];
                if (!std::getline(std::cin, line) ||
                    !(std::istringstream(line) >> carrier >> v[0] >> v[1] >> v[2] >> v[3])) {
                    std::cerr << "channel_estimator_gen: symbol " << symbol_index
                              << " short at pilot " << i << '\n';
                    return 1;
                }
                PilotSymbol p;
                p.subcarrier_index = carrier;
                p.inverse_reference =
                    sample_t(static_cast<int16_t>(v[0]), static_cast<int16_t>(v[1]));
                p.received_value = sample_t(static_cast<int16_t>(v[2]), static_cast<int16_t>(v[3]));
                p.type = atsc3::ofdm::PilotType::SCATTERED;
                pilots.push_back(p);
            }
            const auto h = estimator->estimate(pilots, symbol_index);
            std::cout << "S " << symbol_index << ' ' << h.h_estimate.size() << '\n';
            for (const auto& s : h.h_estimate) {
                std::cout << s.real() << ' ' << s.imag() << '\n';
            }
        } else {
            std::cerr << "channel_estimator_gen: unknown line: " << line << '\n';
            return 1;
        }
    }
    return 0;
}
