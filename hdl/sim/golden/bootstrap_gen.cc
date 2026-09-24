// bootstrap_gen.cc — golden-vector CLI for hdl/rtl/sync/bootstrap_detector.v
//
// Links the real atsc3_lib (lib/sync/bootstrap_detector.h/.cc, built with
// ATSC3_FIXED_POINT=ON) so cocotb's bit-exact comparison is always against
// the actual golden model. Same stdin/stdout text-protocol approach as
// cordic_gen.cc.
//
// Input (stdin):
//   C <sample_rate_hz> <threshold_q15> <averaging_window>   -- first line
//   <re> <im>                                              -- one per sample
//
// threshold_q15 is the raw register value; it is passed to the C++ as
// threshold_q15 / 32768.0, which float_to_q15() maps back to exactly the
// same integer (both conversions are exact for any int16 value).
//
// Output (stdout), per input sample, in order:
//   M <smoothed_metric> <cfo_hz>
//       get_current_metric() * 32768 and get_current_cfo_hz(), both exact
//       integers -- matches the RTL's mon_metric/mon_cfo_hz outputs.
//   D <sample_index> <cfo_hz> <metric>
//       only on samples where process() reports a detection; metric is
//       BootstrapDetection::metric * 32768, matching the RTL status word.
//
// Usage: bootstrap_gen < stimulus.txt > results.txt

#include "sync/bootstrap_detector.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

int main() {
    using atsc3::sample_t;
    using atsc3::sync::BootstrapConfig;
    using atsc3::sync::BootstrapDetector;

    std::string line;
    if (!std::getline(std::cin, line)) {
        std::cerr << "bootstrap_gen: missing config line\n";
        return 1;
    }
    std::istringstream cfg_in(line);
    char tag = 0;
    uint32_t sample_rate_hz = 0;
    int threshold_q15 = 0;
    uint32_t averaging_window = 0;
    if (!(cfg_in >> tag >> sample_rate_hz >> threshold_q15 >> averaging_window) || tag != 'C') {
        std::cerr << "bootstrap_gen: bad config line: " << line << '\n';
        return 1;
    }

    BootstrapConfig config;
    config.sample_rate_hz = static_cast<double>(sample_rate_hz);
    config.threshold = static_cast<double>(threshold_q15) / 32768.0;
    config.averaging_window = averaging_window;
    BootstrapDetector detector(config);

    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        int re = 0;
        int im = 0;
        if (!(iss >> re >> im)) {
            std::cerr << "bootstrap_gen: bad sample line: " << line << '\n';
            return 1;
        }

        auto det = detector.process(sample_t(static_cast<int16_t>(re), static_cast<int16_t>(im)));

        std::cout << "M " << std::llround(detector.get_current_metric() * 32768.0) << ' '
                  << std::llround(detector.get_current_cfo_hz()) << '\n';
        if (det.detected) {
            std::cout << "D " << det.sample_index << ' ' << std::llround(det.cfo_hz) << ' '
                      << std::llround(det.metric * 32768.0) << '\n';
        }
    }
    return 0;
}
