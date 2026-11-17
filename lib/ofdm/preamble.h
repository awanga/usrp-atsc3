// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// Preamble symbol layout — A/322 Sections 7.2.5, 7.2.6.2, 8.1.4, 8.1.6
//
// The preamble's FFT size, guard interval and pilot spacing Dx come from
// the bootstrap's preamble_structure (Table H.1.1). Its carriers
// (relative index k = 0 .. NoC - 1, NoC = NoCmax - Cred_coeff x Cunit) are
//   - preamble pilots: k mod Dx == 0 (Dy = 1, every preamble symbol);
//   - common continual pilots (Section 8.1.4.1) inside NoC;
//   - no edge pilots (Section 8.1.5 excludes preamble symbols);
//   - data cells: the rest, minus the tone reservation carriers when the
//     frame enables TR (never on the first preamble symbol).
// The first preamble symbol always uses the minimum NoC (Cred_coeff 4,
// Section 7.2.5.1); L1B_preamble_reduced_carriers gives the others'.
// Data cells are listed in increasing k, the order the frequency
// interleaver's output is mapped in (Section 7.2.5.3).
//
// Pilot values: 2 A (1/2 - r_k) on the real axis, r_k the Section 8.1.2
// reference sequence (pilot_reference_sequence()), A from Table 8.6 for
// preamble pilots and A_CP = 8.52 dB (Table 8.5) for continual pilots.
//
// Reference: ATSC A/322:2026 Sections 7.2.3, 7.2.5, 7.2.6.2, 8.1, Tables
// 7.1, 7.2, 8.4-8.6, H.1.1

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace ofdm {

struct PreambleLayout {
    size_t fft_size = 0;
    size_t noc = 0;
    size_t dx = 0;
    std::vector<uint32_t> data;       // relative carrier indices, ascending
    std::vector<uint32_t> pilots;     // k mod Dx == 0
    std::vector<uint32_t> continual;  // common CPs that are not preamble pilots
};

// Section 7.2.3 / Table 7.1: NoC for an FFT size and Cred_coeff (0..4);
// 0 if either is out of range.
size_t num_carriers(size_t fft_size, unsigned cred_coeff);

// Layout of a preamble symbol without tone reservation. Returns false for
// an FFT size / Dx pair that Table H.1.1 does not use, or Cred_coeff > 4.
bool preamble_layout(size_t fft_size, size_t dx, unsigned cred_coeff, PreambleLayout* out);

// Table 8.6: preamble pilot power in dB (amplitude 10^(dB/20)). False for
// a pair that Table H.1.1 does not use.
bool preamble_pilot_power_db(size_t fft_size, size_t dx, double* db);

// Table 8.5: common continual pilot power, every FFT size.
constexpr double kContinualPilotPowerDb = 8.52;

}  // namespace ofdm
}  // namespace atsc3
