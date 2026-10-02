// pilot_extractor_gen.cc — golden-vector CLI for hdl/rtl/ofdm/pilot_extractor.v
//
// Links the real atsc3_lib (lib/ofdm/pilot_extractor.h/.cc, built with
// ATSC3_FIXED_POINT=ON): each symbol goes through PilotExtractor::extract(),
// whose output order (ascending carrier, one record per carrier,
// SCATTERED over CONTINUAL over EDGE) is the RTL's output beat order.
//
// Input (stdin):
//   C <fft_size> <pattern 1..8> <num_active_carriers, 0 = default>
//   S <symbol_index>             -- then fft_size lines of <re> <im>
//   ...                          -- repeated per symbol
//
// Output (stdout), per symbol:
//   S <symbol_index>
//   <carrier> <type 0/1/2> <ref_re> <ref_im> <rx_re> <rx_im>   -- per pilot
//
// Usage: pilot_extractor_gen < stimulus.txt > results.txt

#include "ofdm/pilot_extractor.h"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main() {
    using atsc3::sample_t;
    using atsc3::ofdm::PilotExtractor;
    using atsc3::ofdm::PilotExtractorConfig;
    using atsc3::ofdm::PilotPattern;

    std::string line;
    if (!std::getline(std::cin, line)) {
        std::cerr << "pilot_extractor_gen: missing config line\n";
        return 1;
    }
    std::istringstream cfg_in(line);
    char tag = 0;
    size_t fft_size = 0;
    int pattern = 0;
    size_t num_active = 0;
    if (!(cfg_in >> tag >> fft_size >> pattern >> num_active) || tag != 'C' || pattern < 1 ||
        pattern > 8 || num_active > fft_size) {
        std::cerr << "pilot_extractor_gen: bad config line: " << line << '\n';
        return 1;
    }

    PilotExtractorConfig config;
    config.fft_size = fft_size;
    config.pattern = static_cast<PilotPattern>(pattern);
    config.num_active_carriers = num_active;
    PilotExtractor extractor(config);

    std::vector<sample_t> symbol(fft_size);
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream hdr(line);
        size_t symbol_index = 0;
        if (!(hdr >> tag >> symbol_index) || tag != 'S') {
            std::cerr << "pilot_extractor_gen: expected symbol header, got: " << line << '\n';
            return 1;
        }
        for (size_t i = 0; i < fft_size; ++i) {
            int re = 0;
            int im = 0;
            if (!std::getline(std::cin, line) || !(std::istringstream(line) >> re >> im)) {
                std::cerr << "pilot_extractor_gen: symbol " << symbol_index << " short at sample "
                          << i << '\n';
                return 1;
            }
            symbol[i] = sample_t(static_cast<int16_t>(re), static_cast<int16_t>(im));
        }

        std::cout << "S " << symbol_index << '\n';
        for (const auto& p : extractor.extract(symbol.data(), symbol_index)) {
            std::cout << p.subcarrier_index << ' ' << static_cast<int>(p.type) << ' '
                      << p.reference_value.real() << ' ' << p.reference_value.imag() << ' '
                      << p.received_value.real() << ' ' << p.received_value.imag() << '\n';
        }
    }
    return 0;
}
