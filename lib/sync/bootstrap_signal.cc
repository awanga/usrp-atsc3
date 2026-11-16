// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// bootstrap_signal.cc — A/321 bootstrap sequences and signaling fields
//
// Reference: ATSC A/321:2026 Sections 5-6; A/322:2026 Section 9.1, Annex H

#include "bootstrap_signal.h"

#include <cmath>
#include <vector>

namespace atsc3 {
namespace sync {

namespace {

constexpr uint16_t kSeedsMajor0[8] = {0x019D, 0x00ED, 0x01E8, 0x00E8,
                                      0x00FB, 0x0021, 0x0054, 0x00EC};
constexpr uint16_t kSeedsMajor1[8] = {0xF110, 0x3D21, 0xE550, 0xBD49,
                                      0x23CF, 0x0B50, 0x3D3C, 0xA216};

// Table H.1.1 lists preamble_structure 0..159 in 32 groups of five that
// differ only in the L1-Basic FEC mode (value mod 5 + 1).
struct PreambleGroup {
    uint16_t fft;
    uint16_t gi;
    uint8_t dx;
};
constexpr PreambleGroup kPreambleGroups[32] = {
    {8192, 192, 16},  {8192, 384, 8},   {8192, 512, 6},    {8192, 768, 4},   {8192, 1024, 3},
    {8192, 1536, 4},  {8192, 2048, 3},  {16384, 192, 32},  {16384, 384, 16}, {16384, 512, 12},
    {16384, 768, 8},  {16384, 1024, 6}, {16384, 1536, 4},  {16384, 2048, 3}, {16384, 2432, 3},
    {16384, 3072, 4}, {16384, 3648, 4}, {16384, 4096, 3},  {32768, 192, 32}, {32768, 384, 32},
    {32768, 512, 24}, {32768, 768, 16}, {32768, 1024, 12}, {32768, 1536, 8}, {32768, 2048, 6},
    {32768, 2432, 6}, {32768, 3072, 8}, {32768, 3072, 3},  {32768, 3648, 8}, {32768, 3648, 3},
    {32768, 4096, 3}, {32768, 4864, 3},
};

}  // namespace

bool bootstrap_zc_root(int major_version, unsigned* root) {
    switch (major_version) {
        case 0:
            *root = 137;
            return true;
        case 1:
            *root = 197;
            return true;
        default:
            return false;
    }
}

bool bootstrap_pn_seed(int major_version, int minor_version, uint16_t* seed) {
    if (minor_version < 0 || minor_version > 7) {
        return false;
    }
    switch (major_version) {
        case 0:
            *seed = kSeedsMajor0[minor_version];
            return true;
        case 1:
            *seed = kSeedsMajor1[minor_version];
            return true;
        default:
            return false;
    }
}

// Figure 5.2: a Fibonacci LFSR shifting right; the output is r0, and r15
// takes the XOR of the registers tapped by g15, g14, g1 and g0 of
// g(x) = x^16 + x^15 + x^14 + x + 1.
void bootstrap_pn_sequence(uint16_t seed, size_t count, uint8_t* p) {
    uint16_t r = seed;
    for (size_t k = 0; k < count; k++) {
        p[k] = r & 1u;
        unsigned fb = (r ^ (r >> 1) ^ (r >> 14) ^ (r >> 15)) & 1u;
        r = static_cast<uint16_t>((r >> 1) | (fb << 15));
    }
}

bool bootstrap_reference(int major_version, int minor_version, size_t symbol,
                         std::complex<float>* bins) {
    unsigned q = 0;
    uint16_t seed = 0;
    if (!bootstrap_zc_root(major_version, &q) ||
        !bootstrap_pn_seed(major_version, minor_version, &seed)) {
        return false;
    }
    constexpr size_t nh = kBootstrapHalfWidth;
    constexpr uint64_t nzc = kBootstrapZcLength;
    // Symbol n uses c(n*N_H) .. c(n*N_H + N_H - 1).
    std::vector<uint8_t> p((symbol + 1) * nh);
    bootstrap_pn_sequence(seed, p.size(), p.data());
    const uint8_t* c = p.data() + symbol * nh;

    for (size_t i = 0; i < kBootstrapFftSize; i++) {
        bins[i] = 0.0f;
    }
    for (size_t m = 1; m <= nh; m++) {
        // c((n+1)N_H + k) for k = -m and c((n+1)N_H - k) for k = +m both
        // index c(n*N_H + N_H - m).
        float sign = c[nh - m] ? -1.0f : 1.0f;
        for (int s = -1; s <= 1; s += 2) {
            long k = s * static_cast<long>(m);
            uint64_t zi = static_cast<uint64_t>(k + static_cast<long>(nh));
            // z_q(i) = exp(-j pi q i (i+1) / N_ZC); reduce the phase in
            // integers so the angle is exact before the trig call.
            uint64_t ph = (q * zi * (zi + 1)) % (2 * nzc);
            double angle = -M_PI * static_cast<double>(ph) / static_cast<double>(nzc);
            bins[static_cast<size_t>(k + static_cast<long>(kBootstrapFftSize)) %
                 kBootstrapFftSize] =
                std::complex<float>(static_cast<float>(sign * std::cos(angle)),
                                    static_cast<float>(sign * std::sin(angle)));
        }
    }
    return true;
}

unsigned bootstrap_gray_demap(unsigned relative_shift, unsigned n_bits) {
    auto m = [relative_shift](unsigned i) { return (relative_shift >> i) & 1u; };
    unsigned value = m(10);
    for (unsigned i = 1; i < n_bits; i++) {
        value = (value << 1) | (m(11 - i) ^ m(10 - i));
    }
    return value;
}

bool parse_bootstrap_signaling(const uint8_t fields[3], BootstrapSignaling* out) {
    out->ea_wake_up_1 = (fields[0] >> 7) & 1u;
    out->min_time_to_next = (fields[0] >> 2) & 0x1Fu;
    out->system_bandwidth = fields[0] & 0x3u;
    out->ea_wake_up_2 = (fields[1] >> 7) & 1u;
    out->bsr_coefficient = fields[1] & 0x7Fu;
    out->preamble_structure = fields[2];

    PreambleStructure ps;
    bool bsr_ok =
        out->bsr_coefficient == 2 || out->bsr_coefficient == 5 || out->bsr_coefficient == 8;
    return out->min_time_to_next != 31 && out->system_bandwidth != 3 && bsr_ok &&
           preamble_structure_params(out->preamble_structure, &ps);
}

unsigned bootstrap_min_time_to_next_ms(unsigned x) {
    if (x < 8) {
        return 50 * x + 50;
    }
    if (x < 16) {
        return 100 * (x - 8) + 500;
    }
    if (x < 24) {
        return 200 * (x - 16) + 1300;
    }
    return 400 * (x - 24) + 2900;
}

double bootstrap_post_sample_rate_hz(unsigned bsr_coefficient) {
    return (bsr_coefficient + 16.0) * 0.384e6;
}

bool preamble_structure_params(unsigned preamble_structure, PreambleStructure* out) {
    if (preamble_structure >= 5 * 32) {
        return false;
    }
    const PreambleGroup& g = kPreambleGroups[preamble_structure / 5];
    out->fft_size = g.fft;
    out->guard_interval = g.gi;
    out->pilot_dx = g.dx;
    out->l1_basic_mode = static_cast<int>(preamble_structure % 5) + 1;
    return true;
}

}  // namespace sync
}  // namespace atsc3
