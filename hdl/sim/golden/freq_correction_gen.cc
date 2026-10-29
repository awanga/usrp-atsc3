// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// freq_correction_gen.cc — golden-vector CLI for hdl/rtl/sync/freq_correction.v
//
// Links the real atsc3_lib (lib/sync/freq_correction.h/.cc, built with
// ATSC3_FIXED_POINT=ON). The Hz-to-increment conversion and the fine-CFO
// loop filter are control plane (double precision, run by software); the
// RTL takes the resulting Q1.31 phase increment as a register, so every
// command that changes it echoes the new value for the testbench to
// program. Samples go through process() one at a time, in order.
//
// Input (stdin):
//   C <sample_rate_hz>        -- first line
//   F <coarse_cfo_hz>         -- set_coarse_cfo()
//   U <phase_error_per_sample> -- update_fine_cfo()
//   R                         -- reset_phase()
//   <re> <im>                 -- one input sample
//
// Output (stdout), one line per input line after the first:
//   I <phase_increment_q31>   -- for F and U
//   R                         -- for R
//   <re> <im>                 -- the corrected sample
//
// Usage: freq_correction_gen < stimulus.txt > results.txt

#include "sync/freq_correction.h"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

int main() {
#ifdef ATSC3_FIXED_POINT
    using atsc3::sample_t;
    using atsc3::sync::FreqCorrection;
    using atsc3::sync::FreqCorrectionConfig;

    std::string line;
    if (!std::getline(std::cin, line)) {
        std::cerr << "freq_correction_gen: missing config line\n";
        return 1;
    }
    std::istringstream cfg_in(line);
    char tag = 0;
    double sample_rate = 0.0;
    if (!(cfg_in >> tag >> sample_rate) || tag != 'C' || !(sample_rate > 0.0)) {
        std::cerr << "freq_correction_gen: bad config line: " << line << '\n';
        return 1;
    }

    FreqCorrectionConfig config;
    config.sample_rate_hz = sample_rate;
    FreqCorrection corrector(config);

    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        if (line[0] == 'F' || line[0] == 'U') {
            double value = 0.0;
            if (!(iss >> tag >> value)) {
                std::cerr << "freq_correction_gen: bad command: " << line << '\n';
                return 1;
            }
            if (tag == 'F') {
                corrector.set_coarse_cfo(value);
            } else {
                corrector.update_fine_cfo(value);
            }
            std::cout << "I " << corrector.get_phase_increment_q31() << '\n';
        } else if (line[0] == 'R') {
            corrector.reset_phase();
            std::cout << "R\n";
        } else {
            int re = 0;
            int im = 0;
            if (!(iss >> re >> im)) {
                std::cerr << "freq_correction_gen: bad sample line: " << line << '\n';
                return 1;
            }
            sample_t in(static_cast<int16_t>(re), static_cast<int16_t>(im));
            sample_t out;
            corrector.process(&in, &out, 1);
            std::cout << out.real() << ' ' << out.imag() << '\n';
        }
    }
    return 0;
#else
    std::cerr << "freq_correction_gen requires -DATSC3_FIXED_POINT=ON\n";
    return 1;
#endif
}
