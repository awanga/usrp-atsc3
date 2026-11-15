// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// equalizer_gen.cc — golden-vector CLI for hdl/rtl/channel/equalizer.v
//
// Links the real atsc3_lib (lib/channel/equalizer.h/.cc, built with
// ATSC3_FIXED_POINT=ON); each carrier goes through
// Equalizer::equalize_sample().
//
// Input (stdin):
//   C <mode 0 = ZF, 1 = MMSE> <min_channel_magnitude> <noise_variance>
//       -> K <min_magnitude_sq_q30> <noise_q30>   the integers to program
//   E <y_re> <y_im> <h_re> <h_im>
//       -> <re> <im>                              equalized carrier
//
// Usage: equalizer_gen < stimulus.txt > results.txt

#include "channel/equalizer.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

int main() {
    using atsc3::sample_t;
    using atsc3::channel::EqualizationMode;
    using atsc3::channel::Equalizer;
    using atsc3::channel::EqualizerConfig;

    std::unique_ptr<Equalizer> eq;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream in(line);
        char tag = 0;
        in >> tag;
        if (tag == 'C') {
            int mode = 0;
            EqualizerConfig cfg;
            if (!(in >> mode >> cfg.min_channel_magnitude >> cfg.noise_variance) || mode < 0 ||
                mode > 1) {
                std::cerr << "equalizer_gen: bad config line: " << line << '\n';
                return 1;
            }
            cfg.mode = mode == 0 ? EqualizationMode::ZERO_FORCING : EqualizationMode::MMSE;
            eq = std::make_unique<Equalizer>(cfg);
            std::cout << "K " << eq->min_magnitude_sq_q30() << ' ' << eq->noise_q30() << '\n';
        } else if (tag == 'E') {
            int v[4];
            if (!eq || !(in >> v[0] >> v[1] >> v[2] >> v[3])) {
                std::cerr << "equalizer_gen: bad carrier line: " << line << '\n';
                return 1;
            }
            const sample_t x = eq->equalize_sample(
                sample_t(static_cast<int16_t>(v[0]), static_cast<int16_t>(v[1])),
                sample_t(static_cast<int16_t>(v[2]), static_cast<int16_t>(v[3])));
            std::cout << x.real() << ' ' << x.imag() << '\n';
        } else {
            std::cerr << "equalizer_gen: unknown line: " << line << '\n';
            return 1;
        }
    }
    return 0;
}
