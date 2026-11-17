// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// preamble.cc — A/322 preamble symbol carrier layout
//
// Reference: ATSC A/322:2026 Sections 7.2.3, 7.2.5, 7.2.6.2, 8.1

#include "preamble.h"

#include "pilot_extractor.h"

namespace atsc3 {
namespace ofdm {

namespace {

// Table 8.6 (the pilot power depends only on FFT size and Dx).
struct PilotPower {
    size_t fft;
    size_t dx;
    double db;
};
constexpr PilotPower kPilotPower[] = {
    {8192, 16, 5.30},  {8192, 8, 3.60},   {8192, 6, 2.90},   {8192, 4, 1.80},   {8192, 3, 0.90},
    {16384, 32, 6.80}, {16384, 16, 5.30}, {16384, 12, 4.60}, {16384, 8, 3.60},  {16384, 6, 2.90},
    {16384, 4, 2.10},  {16384, 3, 1.30},  {32768, 32, 6.80}, {32768, 24, 6.20}, {32768, 16, 5.30},
    {32768, 12, 4.60}, {32768, 8, 4.00},  {32768, 6, 3.20},  {32768, 3, 1.30},
};

// Table 7.1: NoCmax and Cunit.
bool carrier_params(size_t fft_size, size_t* noc_max, size_t* c_unit) {
    switch (fft_size) {
        case 8192:
            *noc_max = 6913;
            *c_unit = 96;
            return true;
        case 16384:
            *noc_max = 13825;
            *c_unit = 192;
            return true;
        case 32768:
            *noc_max = 27649;
            *c_unit = 384;
            return true;
        default:
            return false;
    }
}

}  // namespace

size_t num_carriers(size_t fft_size, unsigned cred_coeff) {
    size_t noc_max = 0;
    size_t c_unit = 0;
    if (cred_coeff > 4 || !carrier_params(fft_size, &noc_max, &c_unit)) {
        return 0;
    }
    return noc_max - cred_coeff * c_unit;
}

bool preamble_pilot_power_db(size_t fft_size, size_t dx, double* db) {
    for (const PilotPower& p : kPilotPower) {
        if (p.fft == fft_size && p.dx == dx) {
            *db = p.db;
            return true;
        }
    }
    return false;
}

bool preamble_layout(size_t fft_size, size_t dx, unsigned cred_coeff, PreambleLayout* out) {
    double unused_db = 0.0;
    size_t noc_max = 0;
    size_t c_unit = 0;
    if (cred_coeff > 4 || !preamble_pilot_power_db(fft_size, dx, &unused_db) ||
        !carrier_params(fft_size, &noc_max, &c_unit)) {
        return false;
    }
    const size_t noc = noc_max - cred_coeff * c_unit;
    // Absolute carrier index of relative carrier 0 (Section 7.2.3).
    const size_t shift = cred_coeff * c_unit / 2;

    std::vector<uint8_t> kind(noc, 0);  // 0 data, 1 pilot, 2 continual
    for (size_t k = 0; k < noc; k += dx) {
        kind[k] = 1;
    }
    for (size_t abs_index : common_continual_pilots(fft_size)) {
        // Common CPs never sit on k mod Dx == 0 (Section 8.1.4.1: they
        // avoid every scattered pilot position; checked by the tests).
        if (abs_index >= shift && abs_index - shift < noc) {
            kind[abs_index - shift] = 2;
        }
    }

    *out = PreambleLayout{};
    out->fft_size = fft_size;
    out->noc = noc;
    out->dx = dx;
    for (size_t k = 0; k < noc; ++k) {
        std::vector<uint32_t>& dst =
            kind[k] == 0 ? out->data : (kind[k] == 1 ? out->pilots : out->continual);
        dst.push_back(static_cast<uint32_t>(k));
    }
    return true;
}

}  // namespace ofdm
}  // namespace atsc3
