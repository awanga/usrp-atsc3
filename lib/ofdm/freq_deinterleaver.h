// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// Frequency De-interleaver — inverse of the A/322 Section 7.3 frequency
// interleaver
//
// The interleaver permutes the N_data data cells of one OFDM symbol with an
// address sequence H_l(p) built from three generators (Figures 7.27-7.29):
//   - an (Nr-1)-bit interleaving-sequence LFSR R'_i, restarted every symbol,
//     wired to R_i by Tables 7.12-7.14 (8K/16K: separate wirings for even
//     and odd symbols; 32K: one wiring),
//   - a toggle bit (i mod 2) at bit Nr-1,
//   - an Nr-bit symbol offset G_k (k = floor(l/2)), XORed onto the address,
// with Nr = log2(Mmax) and addresses >= N_data skipped (address check).
// The transmitter writes a[p] = x[H_l(p)] (8K, 16K and odd 32K symbols) or
// a[H_l(p)] = x[p] (even 32K symbols); this class applies the inverse.
//
// l counts the symbols of the first Subframe from the first Preamble symbol,
// and of each later Subframe from its first symbol; reset() restarts it.
// Preamble symbols are always interleaved; data and Subframe Boundary
// symbols only when L1D_frequency_interleaver is set for the Subframe.
//
// AXI4-S Interface Contract:
//   Input:  TDATA=cell (interleaved data cells of one symbol) TVALID TREADY TLAST(symbol)
//   Output: TDATA=cell (de-interleaved data cells)          TVALID TREADY TLAST(symbol)
//
// Reference: ATSC A/322 Section 7.3, Tables 7.11-7.14

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atsc3 {
namespace ofdm {

struct FreqDeinterleaverConfig {
    // FFT size: 8192, 16384 or 32768 (Mmax of Table 7.11)
    size_t fft_size = 8192;
};

class FreqDeinterleaver {
public:
    explicit FreqDeinterleaver(const FreqDeinterleaverConfig& config = FreqDeinterleaverConfig());

    // Returns false for an FFT size outside Table 7.11 (the de-interleaver
    // is then unconfigured and deinterleave() copies nothing).
    bool set_config(const FreqDeinterleaverConfig& config);

    const FreqDeinterleaverConfig& get_config() const {
        return config_;
    }

    bool is_configured() const {
        return !raw_even_.empty();
    }

    // Restart at symbol l = 0: first Preamble symbol of a Frame, or first
    // symbol of each later Subframe.
    void reset() {
        symbol_ = 0;
    }

    size_t symbol_index() const {
        return symbol_;
    }

    // De-interleave the num_cells data cells of the current symbol and
    // advance to the next. num_cells must be at most fft_size. T is any
    // copyable cell type (complex samples, or per-cell LLR groups).
    template <typename T>
    void deinterleave(const T* in, T* out, size_t num_cells);

    // H_l(p) for p < num_cells into h (num_cells entries).
    void interleaving_sequence(size_t l, size_t num_cells, uint32_t* h) const;

private:
    FreqDeinterleaverConfig config_;
    size_t nr_ = 0;
    size_t symbol_ = 0;
    // (i mod 2) 2^(Nr-1) + R_i for i < Mmax, per wiring.
    std::vector<uint32_t> raw_even_;
    std::vector<uint32_t> raw_odd_;
    std::vector<uint32_t> h_;  // current symbol's sequence

    uint32_t symbol_offset(size_t k) const;
};

template <typename T>
void FreqDeinterleaver::deinterleave(const T* in, T* out, size_t num_cells) {
    if (!is_configured() || num_cells > config_.fft_size) {
        return;
    }
    interleaving_sequence(symbol_, num_cells, h_.data());
    bool write_mode = config_.fft_size == 32768 && symbol_ % 2 == 0;
    for (size_t p = 0; p < num_cells; p++) {
        if (write_mode) {
            out[p] = in[h_[p]];  // a[H(p)] = x[p]
        } else {
            out[h_[p]] = in[p];  // a[p] = x[H(p)]
        }
    }
    symbol_++;
}

}  // namespace ofdm
}  // namespace atsc3
