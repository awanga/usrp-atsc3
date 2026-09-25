// cp_removal_gen.cc — golden-vector CLI for hdl/rtl/ofdm/cp_removal.v
//
// Links the real atsc3_lib (lib/ofdm/cp_removal.h/.cc, built with
// ATSC3_FIXED_POINT=ON) so cocotb's bit-exact comparison is always
// against the actual golden model, not a Python reimplementation of
// compute_cp_length()'s arithmetic. Same stdin/stdout text-protocol
// approach as bootstrap_gen.cc/cordic_gen.cc.
//
// The C++ CpRemoval buffers an entire CP+FFT symbol before emitting the
// FFT-length tail via one callback call; the RTL instead streams straight
// through with per-sample TVALID/TREADY (see cp_removal.v's header for
// why these differ in latency but not in emitted value sequence or
// framing). This CLI's output is exactly that value sequence, grouped by
// symbol, for cocotb to compare against the RTL's TLAST-delimited output
// beats -- no timing is implied.
//
// Input (stdin):
//   C <fft_size> <cp_fraction_numerator>   -- first line
//   <re> <im>                              -- one per input sample, until EOF
//
// Output (stdout), one block per complete symbol the golden model emits:
//   S <symbol_index>
//   <re> <im>                              -- fft_size lines, in order
//
// Usage: cp_removal_gen < stimulus.txt > results.txt

#include "ofdm/cp_removal.h"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main() {
    using atsc3::sample_t;
    using atsc3::ofdm::CpRemoval;
    using atsc3::ofdm::CpRemovalConfig;

    std::string line;
    if (!std::getline(std::cin, line)) {
        std::cerr << "cp_removal_gen: missing config line\n";
        return 1;
    }
    std::istringstream cfg_in(line);
    char tag = 0;
    size_t fft_size = 0;
    size_t cp_fraction_numerator = 0;
    if (!(cfg_in >> tag >> fft_size >> cp_fraction_numerator) || tag != 'C') {
        std::cerr << "cp_removal_gen: bad config line: " << line << '\n';
        return 1;
    }

    CpRemovalConfig config;
    config.fft_size = fft_size;
    config.cp_fraction = static_cast<atsc3::ofdm::CpFraction>(cp_fraction_numerator);
    CpRemoval remover(config);

    size_t symbol_index = 0;
    remover.set_output_callback([&](const sample_t* symbol, size_t len) {
        std::cout << "S " << symbol_index << '\n';
        for (size_t i = 0; i < len; ++i) {
            std::cout << symbol[i].real() << ' ' << symbol[i].imag() << '\n';
        }
        ++symbol_index;
    });

    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        int re = 0;
        int im = 0;
        if (!(iss >> re >> im)) {
            std::cerr << "cp_removal_gen: bad sample line: " << line << '\n';
            return 1;
        }
        remover.process(sample_t(static_cast<int16_t>(re), static_cast<int16_t>(im)));
    }
    return 0;
}
