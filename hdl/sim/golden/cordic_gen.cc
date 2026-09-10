// cordic_gen.cc — golden-vector CLI for hdl/rtl/common/cordic.v
//
// Links the real atsc3_lib (lib/dsp/cordic.h/.cc) so cocotb's bit-exact
// comparison is always against the actual golden model, never a
// reimplementation that could drift from it. Reads one stimulus line per
// test case from stdin, writes one result line per case to stdout; the
// same text format is shared between this generator and
// hdl/sim/cocotb/test_cordic.py so neither side independently reinvents
// the waveform.
//
// Stimulus line formats:
//   R <theta_q15>            -- rotation mode, theta in the Q1.15 angle
//                                format (see cordic.h)
//   V <x_q15> <y_q15>        -- vectoring mode
//
// Result line format (one per stimulus line, same order):
//   <out_a> <out_b>
// matching cordic.v's port semantics directly: rotation mode's out_a/out_b
// are cos_theta/sin_theta; vectoring mode's are angle_q15/magnitude. This
// is deliberately the same two-field shape for both modes so the RTL
// testbench doesn't need mode-conditional parsing of the golden output.
//
// Usage: cordic_gen < stimulus.txt > results.txt

#include "cordic.h"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

int main() {
    using atsc3::dsp::cordic_rotate;
    using atsc3::dsp::cordic_vector;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        char mode = 0;
        iss >> mode;

        if (mode == 'R') {
            int theta;
            iss >> theta;
            auto result = cordic_rotate(static_cast<int16_t>(theta));
            std::cout << static_cast<int>(result.cos_theta) << ' '
                      << static_cast<int>(result.sin_theta) << '\n';
        } else if (mode == 'V') {
            int x, y;
            iss >> x >> y;
            auto result = cordic_vector(static_cast<int16_t>(x), static_cast<int16_t>(y));
            std::cout << static_cast<int>(result.angle_q15) << ' ' << result.magnitude << '\n';
        } else {
            std::cerr << "cordic_gen: bad stimulus line: " << line << '\n';
            return 1;
        }
    }
    return 0;
}
