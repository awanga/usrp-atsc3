// timing_recovery_gen.cc — golden-vector CLI for hdl/rtl/sync/timing_recovery.v
//
// Links the real atsc3_lib (lib/sync/timing_recovery.h/.cc, built with
// ATSC3_FIXED_POINT=ON) so cocotb's bit-exact comparison and the RTL's
// coefficient ROM are always sourced from the actual golden model, never
// a Python/offline reimplementation of the raised-cosine/Kaiser-window
// filter design or the loop-gain derivation.
//
// Two independent modes, selected by the first stdin line:
//
//   ROM
//     Dumps the 16x32 Q1.15 polyphase coefficient table, one line per
//     [phase][tap] in row-major (phase-major) order:
//       <phase> <tap> <coeff_q15>
//     Used once to generate hdl/rtl/sync/timing_recovery_coeffs.vh (a
//     `define-per-entry file `included by timing_recovery.v's ROM
//     initial block) -- see hdl/rtl/sync/README_coeffs.md generation
//     note in that file's header.
//
//   I
//     followed by:
//   B <re0> <im0> ... <re127> <im127>  -- load the 128-entry ring buffer
//   Q <phase> <base_idx>               -- one interpolate() call
//     Each Q prints "R <re> <im>": PolyphaseInterpolator::interpolate()
//     on the loaded buffer at base_idx with mu = phase/16 (exactly the
//     phase polyphase_fir.v selects), buffer size 128. Feeds
//     test_polyphase_fir.py, which therefore also checks that the
//     generated coefficient ROM matches the running filter design.
//
//   C <sample_rate_hz> <symbol_rate_millihz> <loop_bandwidth_hz>
//     <loop_damping_milli> <samples_per_symbol> <initial_offset_q15>
//     <locked_at_start:0|1>
//     followed by:
//   <re> <im>              -- one input sample per line, until EOF
//   L <0|1>                -- change set_locked() before the next sample
//
//     First prints:
//       G <kp_q15> <ki_q15>
//     (compute_loop_gains()'s one-time output for this config -- the RTL
//     takes these as direct registers rather than re-deriving the
//     division-heavy loop design per cycle). Then, per input sample:
//       M <mu_q16> <timing_error_q15>
//         get_timing_offset()*65536 and get_timing_error()*32768,
//         both exact integers -- matches the RTL's mon_mu_q16/
//         mon_timing_error_q15 outputs, updated every accepted sample
//         regardless of whether a symbol boundary fires this sample.
//       S <re> <im> <mu_q16>
//         only on samples where a symbol was emitted (symbol_callback_
//         fired), i.e. every samples_per_symbol input samples once the
//         buffer has filled -- matches the RTL's m_axis output beat.
//
// Usage: timing_recovery_gen < stimulus.txt > results.txt

#include "sync/timing_recovery.h"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using atsc3::sample_t;
using atsc3::sync::PolyphaseConfig;
using atsc3::sync::PolyphaseInterpolator;
using atsc3::sync::TimingRecovery;
using atsc3::sync::TimingRecoveryConfig;

namespace {

int run_rom_dump() {
#ifdef ATSC3_FIXED_POINT
    PolyphaseConfig config;  // defaults: num_phases=16, taps_per_phase=32
    PolyphaseInterpolator interp(config);
    const auto& coeffs = interp.get_coeffs();
    for (size_t p = 0; p < coeffs.size(); ++p) {
        for (size_t t = 0; t < coeffs[p].size(); ++t) {
            std::cout << p << ' ' << t << ' ' << coeffs[p][t] << '\n';
        }
    }
    return 0;
#else
    // This CLI exists to feed the HDL port (fixed-point-only, see
    // hdl/docs/q_format_notes.md); CMake also builds it in the default
    // float configuration (same as bootstrap_gen/cp_removal_gen), where
    // there is nothing meaningful for it to do.
    std::cerr << "timing_recovery_gen: requires an ATSC3_FIXED_POINT=ON build\n";
    return 1;
#endif
}

int run_interpolate() {
#ifdef ATSC3_FIXED_POINT
    constexpr size_t kBufSize = 128;
    PolyphaseConfig config;
    PolyphaseInterpolator interp(config);
    std::vector<sample_t> buf(kBufSize);
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream iss(line);
        char tag = 0;
        iss >> tag;
        if (tag == 'B') {
            for (size_t i = 0; i < kBufSize; ++i) {
                int re = 0;
                int im = 0;
                if (!(iss >> re >> im)) {
                    std::cerr << "timing_recovery_gen: short B line\n";
                    return 1;
                }
                buf[i] = sample_t(static_cast<int16_t>(re), static_cast<int16_t>(im));
            }
        } else if (tag == 'Q') {
            unsigned phase = 0;
            size_t base_idx = 0;
            if (!(iss >> phase >> base_idx) || phase >= config.num_phases ||
                base_idx >= kBufSize) {
                std::cerr << "timing_recovery_gen: bad Q line: " << line << '\n';
                return 1;
            }
            double mu = static_cast<double>(phase) / static_cast<double>(config.num_phases);
            sample_t y = interp.interpolate(buf.data(), base_idx, mu, kBufSize);
            std::cout << "R " << y.real() << ' ' << y.imag() << '\n';
        } else if (!line.empty()) {
            std::cerr << "timing_recovery_gen: bad line: " << line << '\n';
            return 1;
        }
    }
    return 0;
#else
    std::cerr << "timing_recovery_gen: requires an ATSC3_FIXED_POINT=ON build\n";
    return 1;
#endif
}

int run_capture(const std::string& first_line) {
#ifndef ATSC3_FIXED_POINT
    // See run_rom_dump()'s #else branch: this CLI is only meaningful
    // against an ATSC3_FIXED_POINT=ON build.
    (void)first_line;
    std::cerr << "timing_recovery_gen: requires an ATSC3_FIXED_POINT=ON build\n";
    return 1;
#else
    std::istringstream cfg_in(first_line);
    char tag = 0;
    uint32_t sample_rate_hz = 0;
    uint32_t symbol_rate_millihz = 0;
    uint32_t loop_bandwidth_hz = 0;
    uint32_t loop_damping_milli = 0;
    uint32_t samples_per_symbol = 0;
    int initial_offset_q15 = 0;
    int locked_at_start = 0;
    if (!(cfg_in >> tag >> sample_rate_hz >> symbol_rate_millihz >> loop_bandwidth_hz >>
          loop_damping_milli >> samples_per_symbol >> initial_offset_q15 >> locked_at_start) ||
        tag != 'C') {
        std::cerr << "timing_recovery_gen: bad config line: " << first_line << '\n';
        return 1;
    }

    TimingRecoveryConfig config;
    config.sample_rate_hz = static_cast<double>(sample_rate_hz);
    config.symbol_rate_hz = static_cast<double>(symbol_rate_millihz) / 1000.0;
    config.loop_bandwidth_hz = static_cast<double>(loop_bandwidth_hz);
    config.loop_damping = static_cast<double>(loop_damping_milli) / 1000.0;
    config.polyphase.samples_per_symbol = samples_per_symbol;
    config.initial_offset = static_cast<double>(initial_offset_q15) / 32768.0;

    TimingRecovery timing(config);
    timing.set_locked(locked_at_start != 0);

    std::cout << "G " << timing.get_kp_q15() << ' ' << timing.get_ki_q15() << '\n';

    timing.set_symbol_callback([&](const sample_t& sym, double mu) {
        long mu_q16 = std::lround(mu * 65536.0);
        std::cout << "S " << sym.real() << ' ' << sym.imag() << ' ' << mu_q16 << '\n';
    });

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        if (line[0] == 'L') {
            std::istringstream iss(line);
            char c = 0;
            int locked = 0;
            iss >> c >> locked;
            timing.set_locked(locked != 0);
            continue;
        }
        std::istringstream iss(line);
        int re = 0;
        int im = 0;
        if (!(iss >> re >> im)) {
            std::cerr << "timing_recovery_gen: bad sample line: " << line << '\n';
            return 1;
        }
        timing.process_sample(sample_t(static_cast<int16_t>(re), static_cast<int16_t>(im)));

        long mu_q16 = std::lround(timing.get_timing_offset() * 65536.0);
        long err_q15 = std::lround(timing.get_timing_error() * 32768.0);
        std::cout << "M " << mu_q16 << ' ' << err_q15 << '\n';
    }
    return 0;
#endif  // ATSC3_FIXED_POINT
}

}  // namespace

int main() {
    std::string first_line;
    if (!std::getline(std::cin, first_line)) {
        std::cerr << "timing_recovery_gen: missing first line\n";
        return 1;
    }
    if (first_line == "ROM") {
        return run_rom_dump();
    }
    if (first_line == "I") {
        return run_interpolate();
    }
    return run_capture(first_line);
}
