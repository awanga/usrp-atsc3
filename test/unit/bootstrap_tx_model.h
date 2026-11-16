// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// Test-side bootstrap transmitter, written from A/321 Sections 5.2-5.4 and
// kept independent of lib/sync: its own PN register array (Figure 5.2), ZC
// formula (5.2.1), subcarrier mapping (5.2.3), direct IFFT sum with the
// 1/sqrt(N_ZC - 1) scaling (5.2.4), Gray mapping (5.3.2), absolute cyclic
// shift (5.3.3) and the CAB / BCA time-domain formulas (5.4.1, 5.4.2).

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace test {

struct BootstrapTxModel {
    static constexpr int kNfft = 2048;
    static constexpr int kNzc = 1499;
    static constexpr int kNh = (kNzc - 1) / 2;
    static constexpr int kSymbol = 3072;

    // Figure 5.2 with registers r[0..15] and g = {g16, ..., g0}.
    static std::vector<int> pn(uint16_t seed, int count) {
        const int g[17] = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1};
        int r[16];
        for (int i = 0; i < 16; i++) {
            r[i] = (seed >> i) & 1;
        }
        std::vector<int> out;
        for (int k = 0; k < count; k++) {
            out.push_back(r[0]);
            int fb = 0;
            for (int i = 0; i < 16; i++) {
                fb ^= g[16 - i] & r[i];
            }
            for (int i = 0; i < 15; i++) {
                r[i] = r[i + 1];
            }
            r[15] = fb;
        }
        return out;
    }

    // s_n(k) for k = -N_H .. N_H (index k + N_H), Section 5.2.3, without
    // the final-symbol inversion.
    static std::vector<std::complex<double>> freq(int q, uint16_t seed, int n) {
        std::vector<int> p = pn(seed, (n + 2) * kNh);
        auto c = [&p](int k) { return 1.0 - 2.0 * p[k]; };
        auto z = [q](int k) {
            return std::exp(std::complex<double>(0.0, -M_PI * q * k * (k + 1.0) / kNzc));
        };
        std::vector<std::complex<double>> s(kNzc, 0.0);
        for (int k = -kNh; k <= -1; k++) {
            s[k + kNh] = z(k + kNh) * c((n + 1) * kNh + k);
        }
        for (int k = 1; k <= kNh; k++) {
            s[k + kNh] = z(k + kNh) * c((n + 1) * kNh - k);
        }
        return s;
    }

    // Section 5.3.2: relative cyclic shift from bits b_0..b_{nb-1}.
    static int gray_map(const std::vector<int>& b) {
        int nb = static_cast<int>(b.size());
        int shift = 0;
        for (int i = 0; i <= 10; i++) {
            int m = 0;
            if (i > 10 - nb) {
                for (int k = 0; k <= 10 - i; k++) {
                    m ^= b[k];
                }
            } else if (i == 10 - nb) {
                m = 1;
            }
            shift |= m << i;
        }
        return shift;
    }

    // The whole bootstrap: symbols 0..N_S-1, symbol n >= 1 carrying
    // bits[n-1] (b_0 first). Unit-power-per-subcarrier scaling of 5.2.4.
    static std::vector<std::complex<double>> waveform(int q, uint16_t seed,
                                                      const std::vector<std::vector<int>>& bits) {
        const int ns = static_cast<int>(bits.size()) + 1;
        std::vector<std::complex<double>> tw(kNfft);
        for (int i = 0; i < kNfft; i++) {
            tw[i] = std::exp(std::complex<double>(0.0, 2.0 * M_PI * i / kNfft));
        }
        std::vector<std::complex<double>> out;
        int m_abs = 0;
        for (int n = 0; n < ns; n++) {
            std::vector<std::complex<double>> s = freq(q, seed, n);
            double sign = n == ns - 1 ? -1.0 : 1.0;
            // A~_n(t) = 1/sqrt(N_ZC - 1) * sum_k s~_n(k) exp(j 2 pi k f_d t),
            // t in units of T_S so k f_d t = k t / N_FFT.
            std::vector<std::complex<double>> a_tilde(kNfft);
            for (int t = 0; t < kNfft; t++) {
                std::complex<double> acc = 0.0;
                for (int k = -kNh; k <= kNh; k++) {
                    if (k == 0) {
                        continue;
                    }
                    int idx = ((k * t) % kNfft + kNfft) % kNfft;
                    acc += sign * s[k + kNh] * tw[idx];
                }
                a_tilde[t] = acc / std::sqrt(kNzc - 1.0);
            }
            if (n > 0) {
                m_abs = (m_abs + gray_map(bits[n - 1])) % kNfft;
            }
            auto a = [&](int t) { return a_tilde[(t + m_abs) % kNfft]; };
            auto rot = [&](double t) {
                return std::exp(std::complex<double>(0.0, 2.0 * M_PI * t / kNfft));
            };
            for (int t = 0; t < kSymbol; t++) {
                std::complex<double> v;
                if (n == 0) {  // CAB, Section 5.4.1
                    if (t < 520) {
                        v = a(t + 1528);
                    } else if (t < 2568) {
                        v = a(t - 520);
                    } else {
                        v = a(t - 1024) * rot(t);
                    }
                } else {  // BCA, Section 5.4.2
                    if (t < 504) {
                        v = a(t + 1528) * rot(-(t - 520.0));
                    } else if (t < 1024) {
                        v = a(t + 1024);
                    } else {
                        v = a(t - 1024);
                    }
                }
                out.push_back(v);
            }
        }
        return out;
    }

    // Version 0.0 symbol fields (Tables 6.2, 6.4, 6.5) as 8 bits each,
    // b_0 = MSB of the field.
    static std::vector<std::vector<int>> fields_to_bits(const std::array<int, 3>& fields) {
        std::vector<std::vector<int>> bits(3, std::vector<int>(8));
        for (int s = 0; s < 3; s++) {
            for (int i = 0; i < 8; i++) {
                bits[s][i] = (fields[s] >> (7 - i)) & 1;
            }
        }
        return bits;
    }
};

}  // namespace test
}  // namespace atsc3
