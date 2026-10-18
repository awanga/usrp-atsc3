// frame_sync_gen.cc — golden-vector CLI for hdl/rtl/sync/frame_sync.v
//
// Links the real atsc3_lib (lib/sync/frame_sync.h/.cc, built with
// ATSC3_FIXED_POINT=ON). Same stdin/stdout text protocol as the other
// golden CLIs.
//
// FrameSync::process() is called once per input chunk; the RTL buffers a
// TLAST-terminated chunk and runs the same procedure over it, so a chunk
// here is exactly the RTL's TLAST-delimited packet.
//
// Input (stdin), one command per line:
//   P <fft> <cp> <sym_per_sub> <sub_per_frame> <thr_q15> <acquire> <loss>
//     <holdover_frames> <search_window>
//         (re)configure: builds a fresh FrameSync with these parameters
//   B <sample_index> <confidence_q15>
//         set_bootstrap_offset(); confidence_q15 / 32768.0 maps back to
//         exactly the same Q1.15 value
//   K <n>
//         chunk of n samples; the next n lines are "<re> <im>"
//   Q <n> <offset>
//         compute_correlation() on the next n sample lines, no state change
//   R
//         reset()
//   Z
//         dump the 13-entry preamble reference as "Z <k> <re> <im>" lines
//
// Output (stdout):
//   E <type> <sample_index> <frame> <subframe> <symbol> <confidence_q15>
//         one per FrameEvent, in order
//   T <state> <frame> <expected_boundary> <confidence_q15> <samples_to_boundary>
//         after each K and B and R: state (0 SEARCHING, 1 ACQUIRING,
//         2 LOCKED, 3 HOLDOVER), frame number, confidence in Q1.15
//   C <ratio_q15>
//         answer to Q

#include "sync/frame_sync.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using atsc3::sample_t;
using atsc3::sync::FrameEvent;
using atsc3::sync::FrameSync;
using atsc3::sync::FrameSyncConfig;
using atsc3::sync::FrameSyncState;

bool read_samples(size_t n, std::vector<sample_t>& out) {
    std::string line;
    out.clear();
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        int re = 0;
        int im = 0;
        if (!std::getline(std::cin, line)) {
            return false;
        }
        std::istringstream iss(line);
        if (!(iss >> re >> im)) {
            return false;
        }
        out.emplace_back(static_cast<int16_t>(re), static_cast<int16_t>(im));
    }
    return true;
}

long to_q15(double v) {
    return std::lround(v * 32768.0);
}

void print_state(const FrameSync& sync) {
    std::cout << "T " << static_cast<int>(sync.get_state()) << ' ' << sync.get_frame_number() << ' '
              << sync.expected_boundary() << ' ' << to_q15(sync.get_confidence()) << ' '
              << sync.get_samples_to_boundary() << '\n';
}

}  // namespace

int main() {
    std::unique_ptr<FrameSync> sync;
    std::string line;

    auto make = [&](const FrameSyncConfig& cfg) {
        sync = std::make_unique<FrameSync>(cfg);
        sync->set_frame_callback([](const FrameEvent& ev) {
            std::cout << "E " << static_cast<int>(ev.type) << ' ' << ev.sample_index << ' '
                      << ev.frame_number << ' ' << ev.subframe_number << ' ' << ev.symbol_number
                      << ' ' << to_q15(ev.confidence) << '\n';
        });
    };
    make(FrameSyncConfig());

    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        char tag = 0;
        iss >> tag;
        if (tag == 'P') {
            FrameSyncConfig cfg;
            size_t fft = 0, cp = 0, sym = 0, sub = 0, acq = 0, loss = 0, hold = 0, win = 0;
            int thr = 0;
            if (!(iss >> fft >> cp >> sym >> sub >> thr >> acq >> loss >> hold >> win)) {
                std::cerr << "frame_sync_gen: bad P line: " << line << '\n';
                return 1;
            }
            cfg.frame_params.fft_size = fft;
            cfg.frame_params.cp_length = cp;
            cfg.frame_params.symbols_per_subframe = sym;
            cfg.frame_params.subframes_per_frame = sub;
            cfg.detection_threshold = static_cast<double>(thr) / 32768.0;
            cfg.lock_acquire_count = acq;
            cfg.lock_loss_count = loss;
            cfg.holdover_frames = hold;
            cfg.search_window = win;
            make(cfg);
        } else if (tag == 'B') {
            size_t idx = 0;
            int conf = 0;
            if (!(iss >> idx >> conf)) {
                std::cerr << "frame_sync_gen: bad B line: " << line << '\n';
                return 1;
            }
            sync->set_bootstrap_offset(idx, static_cast<double>(conf) / 32768.0);
            print_state(*sync);
        } else if (tag == 'K') {
            size_t n = 0;
            std::vector<sample_t> samples;
            if (!(iss >> n) || !read_samples(n, samples)) {
                std::cerr << "frame_sync_gen: bad K chunk: " << line << '\n';
                return 1;
            }
            sync->process(samples.data(), n);
            print_state(*sync);
        } else if (tag == 'Q') {
            size_t n = 0;
            size_t offset = 0;
            std::vector<sample_t> samples;
            if (!(iss >> n >> offset) || !read_samples(n, samples)) {
                std::cerr << "frame_sync_gen: bad Q chunk: " << line << '\n';
                return 1;
            }
            std::cout << "C " << to_q15(sync->compute_correlation(samples.data(), n, offset))
                      << '\n';
        } else if (tag == 'R') {
            sync->reset();
            print_state(*sync);
        } else if (tag == 'Z') {
            const auto& ref = sync->preamble_reference();
            for (size_t k = 13; k < ref.size(); ++k) {
                if (ref[k] != ref[k % 13]) {
                    std::cerr << "frame_sync_gen: reference is not 13-periodic at " << k << '\n';
                    return 1;
                }
            }
            for (size_t k = 0; k < ref.size() && k < 13; ++k) {
                std::cout << "Z " << k << ' ' << ref[k].real() << ' ' << ref[k].imag() << '\n';
            }
        } else {
            std::cerr << "frame_sync_gen: unknown command: " << line << '\n';
            return 1;
        }
    }
    return 0;
}
