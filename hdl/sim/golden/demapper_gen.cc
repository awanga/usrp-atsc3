// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// demapper_gen.cc — golden-vector CLI for hdl/rtl/ofdm/constellation_demapper.v
//
// Links the real atsc3_lib (lib/ofdm/constellation_demapper.h/.cc, built with
// ATSC3_FIXED_POINT=ON); each cell goes through demap_symbol().
//
// Input (stdin):
//   C <modulation 0..11> <code rate 0..11> <noise_variance> <llr_clip>
//       -> K <bits_per_symbol> <llr_scale_q16> <norm_const_q15>
//          the integers to program (norm_const_q15 is 0 for NUC modes)
//   X <re> <im>
//       -> <llr_0> ... <llr_{bits-1}>      one line per cell
//
// Usage: demapper_gen < stimulus.txt > results.txt

#include "ofdm/constellation_demapper.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

int main() {
    using atsc3::sample_t;
    using atsc3::ofdm::ConstellationDemapper;
    using atsc3::ofdm::DemapperConfig;

    std::unique_ptr<ConstellationDemapper> dm;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream in(line);
        char tag = 0;
        in >> tag;
        if (tag == 'C') {
            int mod = 0;
            int rate = 0;
            int clip = 0;
            DemapperConfig cfg;
            if (!(in >> mod >> rate >> cfg.noise_variance >> clip) || mod < 0 || mod > 11 ||
                rate < 0 || rate > 11 || clip < 1 || clip > 127) {
                std::cerr << "demapper_gen: bad config line: " << line << '\n';
                return 1;
            }
            cfg.modulation = static_cast<atsc3::config::Modulation>(mod);
            cfg.code_rate = static_cast<atsc3::config::CodeRate>(rate);
            cfg.llr_clip = static_cast<int8_t>(clip);
            dm = std::make_unique<ConstellationDemapper>(cfg);
            std::cout << "K " << dm->bits_per_symbol() << ' ' << dm->llr_scale_q16() << ' '
                      << dm->norm_const_q15() << '\n';
        } else if (tag == 'X') {
            int re = 0;
            int im = 0;
            if (!dm || !(in >> re >> im)) {
                std::cerr << "demapper_gen: bad cell line: " << line << '\n';
                return 1;
            }
            std::vector<int8_t> llr(dm->bits_per_symbol());
            dm->demap_symbol(sample_t(static_cast<int16_t>(re), static_cast<int16_t>(im)),
                             llr.data());
            for (size_t b = 0; b < llr.size(); ++b) {
                std::cout << (b ? " " : "") << static_cast<int>(llr[b]);
            }
            std::cout << '\n';
        } else {
            std::cerr << "demapper_gen: unknown line: " << line << '\n';
            return 1;
        }
    }
    return 0;
}
