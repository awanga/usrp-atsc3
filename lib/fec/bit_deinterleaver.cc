// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// bit_deinterleaver.cc — inverse A/322 bit interleaver and demultiplexer
//
// Reference: ATSC A/322 Sections 6.2 and 6.3.3, Tables 6.8-6.11, Annex B

#include "bit_deinterleaver.h"

#include "bit_interleaver_tables.h"
#include "ldpc_tables.h"

namespace atsc3 {
namespace fec {

namespace {

constexpr size_t kGroup = 360;

// Tables 6.8 (64800) and 6.9 (16200): one row per eta_MOD = 2, 4, ..., one
// column per code rate 2/15..13/15.
constexpr const char* kTypeLong[6] = {
    "AAAAAAAAAAAA", "AAABAABBAAAA", "AAAAABABBAAB", "AAABBBBABBAB", "AAABABABBBAA", "AAAAABAAAAAA",
};
constexpr const char* kTypeShort[4] = {
    "AAAABBABAAAA",
    "AAAABBABABAB",
    "AAAABBABAAAA",
    "AAAABAAAAABA",
};

}  // namespace

BlockInterleaverType block_interleaver_type(config::CodeRate rate, size_t bits_per_cell,
                                            bool short_codeword) {
    size_t row = bits_per_cell / 2 - 1;
    const char* types = short_codeword ? kTypeShort[row] : kTypeLong[row];
    return types[static_cast<size_t>(rate)] == 'A' ? BlockInterleaverType::TYPE_A
                                                   : BlockInterleaverType::TYPE_B;
}

bool BitDeinterleaver::configure(config::CodeRate rate, config::Modulation modulation,
                                 bool short_codeword) {
    source_.clear();
    const size_t r = static_cast<size_t>(rate);
    const size_t eta = config::Atsc3Config::bits_per_symbol(modulation);
    const size_t mod_index = eta / 2 - 1;
    if (r >= 12 || (short_codeword && eta > 8)) {
        return false;
    }

    const LdpcTable& code = k_ldpc_tables[short_codeword ? 1 : 0][r];
    const size_t ninner = code.ninner;
    const size_t nouter = code.nouter;
    const size_t ngroup = ninner / kGroup;
    const size_t qldpc = (ninner - nouter) / kGroup;
    const uint8_t* pi =
        short_codeword ? k_group_order_short[mod_index][r] : k_group_order_long[mod_index][r];

    // Group-wise interleaver: Y_j = X_pi(j), so group g of U lands at j.
    std::vector<uint32_t> group_position(ngroup);
    for (size_t j = 0; j < ngroup; j++) {
        group_position[pi[j]] = static_cast<uint32_t>(j);
    }

    const BlockInterleaverType type = block_interleaver_type(rate, eta, short_codeword);
    // Table 6.10: Nr1 = floor(Ngroup/Nc) * 360 rows, the rest in Part 2.
    const size_t nr1 = (ngroup / eta) * kGroup;
    const size_t nr2 = ninner / eta - nr1;
    // Table 6.11: Part 1 is whole sets of N_QCB_IG = eta bit groups.
    const size_t npart1 = (ngroup / eta) * eta * kGroup;

    source_.resize(ninner);
    for (size_t k = 0; k < ninner; k++) {
        // Parity interleaver (6.2.1), Type B LDPC codes only:
        // u[Nouter + 360t + s] = lambda[Nouter + Qldpc*s + t].
        size_t u = k;
        if (!code.type_a && k >= nouter) {
            size_t p = k - nouter;
            u = nouter + kGroup * (p % qldpc) + p / qldpc;
        }
        size_t v = kGroup * group_position[u / kGroup] + u % kGroup;

        size_t q;
        if (type == BlockInterleaverType::TYPE_A) {
            // Written column-wise (Part 1 then Part 2), read row-wise.
            size_t col;
            size_t row;
            if (v < eta * nr1) {
                col = v / nr1;
                row = v % nr1;
            } else {
                col = (v - eta * nr1) / nr2;
                row = nr1 + (v - eta * nr1) % nr2;
            }
            q = row * eta + col;
        } else if (v < npart1) {
            // Each set of eta groups written as eta rows of 360, read
            // column-wise one bit per row.
            size_t set = v / (eta * kGroup);
            size_t w = v % (eta * kGroup);
            q = set * eta * kGroup + (w % kGroup) * eta + w / kGroup;
        } else {
            q = v;
        }
        // Demultiplexer (6.3.3): cell s carries q[eta*s] .. q[eta*s + eta-1].
        source_[k] = static_cast<uint32_t>(q);
    }
    return true;
}

void BitDeinterleaver::deinterleave(const int8_t* in, int8_t* out) const {
    for (size_t k = 0; k < source_.size(); k++) {
        out[k] = in[source_[k]];
    }
}

}  // namespace fec
}  // namespace atsc3
