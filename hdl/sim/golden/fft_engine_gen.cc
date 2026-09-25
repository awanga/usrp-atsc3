// fft_engine_gen.cc — golden-vector CLI for hdl/rtl/ofdm/fft_engine.v
//
// Links the real atsc3_lib (lib/ofdm/fft_engine.h/.cc, built with
// ATSC3_FIXED_POINT=ON) so cocotb's bit-exact comparison is always
// against the actual golden model (FixedPointFftEngine's Cooley-Tukey
// radix-2 DIT), never a Python reimplementation. Forward direction only
// -- see fft_engine.v's header on why the RTL has no direction input.
//
// Unlike the streaming golden CLIs (bootstrap_gen, cp_removal_gen,
// timing_recovery_gen), FftEngine::process() takes one whole FFT_SIZE
// block at a time with no streaming state of its own, so this CLI does
// the same: one config line names the size, then exactly FFT_SIZE
// "<re> <im>" input lines, and it prints exactly FFT_SIZE "<re> <im>"
// output lines in return.
//
// Input (stdin):
//   C <fft_size>                -- first line; fft_size in {8192,16384,32768}
//   <re> <im>                   -- exactly fft_size lines
//
// Output (stdout):
//   <re> <im>                   -- exactly fft_size lines, in order
//
// Usage: fft_engine_gen < stimulus.txt > results.txt

#include "ofdm/fft_engine.h"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using atsc3::sample_t;
using atsc3::ofdm::FftConfig;
using atsc3::ofdm::FftDirection;
using atsc3::ofdm::FftEngine;
using atsc3::ofdm::FftSize;

int main() {
    std::string line;
    if (!std::getline(std::cin, line)) {
        std::cerr << "fft_engine_gen: missing config line\n";
        return 1;
    }
    std::istringstream cfg_in(line);
    char tag = 0;
    size_t fft_size = 0;
    if (!(cfg_in >> tag >> fft_size) || tag != 'C') {
        std::cerr << "fft_engine_gen: bad config line: " << line << '\n';
        return 1;
    }

    FftConfig config;
    switch (fft_size) {
        case 8192:
            config.size = FftSize::k8K;
            break;
        case 16384:
            config.size = FftSize::k16K;
            break;
        case 32768:
            config.size = FftSize::k32K;
            break;
        default:
            std::cerr << "fft_engine_gen: unsupported fft_size " << fft_size << '\n';
            return 1;
    }
    config.direction = FftDirection::kForward;

    auto fft = FftEngine::create(config);
    if (!fft) {
        std::cerr << "fft_engine_gen: FftEngine::create() failed\n";
        return 1;
    }

    std::vector<sample_t> in(fft_size);
    std::vector<sample_t> out(fft_size);

    for (size_t i = 0; i < fft_size; ++i) {
        if (!std::getline(std::cin, line)) {
            std::cerr << "fft_engine_gen: expected " << fft_size << " input samples, got " << i
                      << '\n';
            return 1;
        }
        std::istringstream iss(line);
        int re = 0;
        int im = 0;
        if (!(iss >> re >> im)) {
            std::cerr << "fft_engine_gen: bad sample line: " << line << '\n';
            return 1;
        }
        in[i] = sample_t(static_cast<int16_t>(re), static_cast<int16_t>(im));
    }

    fft->process(in.data(), out.data());

    for (size_t i = 0; i < fft_size; ++i) {
        std::cout << out[i].real() << ' ' << out[i].imag() << '\n';
    }
    return 0;
}
