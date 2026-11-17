// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// Bootstrap signal definitions — A/321 Sections 5 and 6, A/322 Section 9.1
//
// The bootstrap runs at a fixed 6.144 MS/s with a 2048-point FFT whatever
// the post-bootstrap waveform. Each symbol is 3072 samples: symbol 0 is CAB
// (C = last 520 samples of A, A = 2048-sample IFFT output, B = last 504
// samples of A shifted up by one subcarrier), the others are BCA. The
// frequency-domain sequence is a Zadoff-Chu sequence (root from the major
// version) times a +-1 PN sequence (seed from the minor version) that runs
// on across symbols, mirrored about the null DC subcarrier. Symbol n >= 1
// carries its signaling bits as a Gray-coded cyclic shift relative to
// symbol n-1; the last symbol is phase inverted.
//
// ATSC 3.0 (A/322) uses bootstrap version 0.0: four symbols, 8 signaling
// bits in each of symbols 1-3.
//
// Reference: ATSC A/321:2026 Sections 5.1-5.4, 6.1; A/322:2026 Section 9.1,
// Table 9.1, Annex H

#include <complex>
#include <cstddef>
#include <cstdint>

namespace atsc3 {
namespace sync {

constexpr double kBootstrapSampleRateHz = 6.144e6;
constexpr size_t kBootstrapFftSize = 2048;
constexpr size_t kBootstrapZcLength = 1499;                           // N_ZC
constexpr size_t kBootstrapHalfWidth = (kBootstrapZcLength - 1) / 2;  // N_H = 749
constexpr size_t kBootstrapPartA = 2048;
constexpr size_t kBootstrapPartB = 504;
constexpr size_t kBootstrapPartC = 520;
constexpr size_t kBootstrapSymbolLength = 3072;
// Offset of part A within a symbol: after C for CAB (symbol 0), after B
// and C for BCA (symbols 1..N_S-1).
constexpr size_t kBootstrapCabOffsetA = kBootstrapPartC;
constexpr size_t kBootstrapBcaOffsetA = kBootstrapPartB + kBootstrapPartC;

// Version 0.0 (A/321 Section 6.1.1.1; A/322 Section 9.1.1).
constexpr size_t kBootstrapSymbols = 4;
constexpr size_t kBootstrapSignalingBits = 8;

// ZC root (Sections 6.1, 6.2) and PN seed {r15..r0} (Tables 6.1, 6.6).
// Return false for an undefined major/minor version.
bool bootstrap_zc_root(int major_version, unsigned* root);
bool bootstrap_pn_seed(int major_version, int minor_version, uint16_t* seed);

// Section 5.2.2: count PN outputs p(0..count-1) from the given seed.
void bootstrap_pn_sequence(uint16_t seed, size_t count, uint8_t* p);

// Section 5.2.3: the frequency-domain sequence s_n(k) of symbol n, without
// the last-symbol phase inversion, in FFT bin order (bin k mod 2048).
// Inactive bins and DC are zero. Unit magnitude on active bins.
bool bootstrap_reference(int major_version, int minor_version, size_t symbol,
                         std::complex<float>* bins);

// Annex A: the n_bits signaling bits b_0..b_{n_bits-1} carried by a
// relative cyclic shift (0..2047), returned as a field value with b_0 as
// its MSB. Valid for 1 <= n_bits <= 10.
unsigned bootstrap_gray_demap(unsigned relative_shift, unsigned n_bits);

// Tables 6.2, 6.4, 6.5, with the A/322 Section 9.1 constraints.
struct BootstrapSignaling {
    uint8_t ea_wake_up_1 = 0;
    uint8_t min_time_to_next = 0;
    uint8_t system_bandwidth = 0;
    uint8_t ea_wake_up_2 = 0;
    uint8_t bsr_coefficient = 0;
    uint8_t preamble_structure = 0;
};

// fields[i] is the 8-bit field value of symbol i + 1. Returns false when a
// value is reserved or not allowed for A/322: min_time_to_next = 31,
// system_bandwidth = 11, bsr_coefficient not in Table 9.1 (2, 5, 8), or a
// reserved preamble_structure. out is filled either way.
bool parse_bootstrap_signaling(const uint8_t fields[3], BootstrapSignaling* out);

// Section 6.1.1.1: lower edge of the signaled interval, in ms. X in 0..31
// (31 is not signaled; T(31) is the upper edge of X = 30's interval).
unsigned bootstrap_min_time_to_next_ms(unsigned x);

// Section 6.1.1.1 (major version 0): (N + 16) x 0.384 MHz.
double bootstrap_post_sample_rate_hz(unsigned bsr_coefficient);

// A/322 Table H.1.1.
struct PreambleStructure {
    size_t fft_size = 0;
    size_t guard_interval = 0;  // samples
    size_t pilot_dx = 0;
    int l1_basic_mode = 0;  // 1..5
};
bool preamble_structure_params(unsigned preamble_structure, PreambleStructure* out);

}  // namespace sync
}  // namespace atsc3
