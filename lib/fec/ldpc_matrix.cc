// ldpc_matrix.cc — ATSC 3.0 LDPC parity check matrices and encoder
//
// Reference: ATSC A/322 Section 6.1.3, Annex A

#include "ldpc_matrix.h"

#include "ldpc_decoder.h"
#include "ldpc_tables.h"

#include <algorithm>
#include <fstream>

namespace atsc3 {
namespace fec {

namespace {

constexpr size_t kGroup = 360;

const LdpcTable* table_for(config::CodeRate rate, bool short_codeword) {
    size_t idx = static_cast<size_t>(rate);
    if (idx >= sizeof(k_ldpc_tables[0]) / sizeof(k_ldpc_tables[0][0])) {
        return nullptr;
    }
    return &k_ldpc_tables[short_codeword ? 1 : 0][idx];
}

size_t parity_length(const LdpcTable& t) {
    return t.ninner - t.nouter;
}

// Parity accumulator fed by bit m of the group whose first bit uses address
// x (Section 6.1.3.1 step ii, Section 6.1.3.2 q(i, j, l)).
size_t accumulator(const LdpcTable& t, size_t x, size_t m) {
    size_t minner = parity_length(t);
    if (!t.type_a) {
        return (x + m * (minner / kGroup)) % minner;
    }
    if (x < t.m1) {
        return (x + m * (t.m1 / kGroup)) % t.m1;
    }
    size_t m2 = minner - t.m1;
    return t.m1 + (x - t.m1 + m * (m2 / kGroup)) % m2;
}

// Codeword column that carries parity accumulator j: Type A interleaves both
// parity parts (Section 6.1.3.1 steps vi and viii); Type B does not.
size_t parity_column(const LdpcTable& t, size_t j) {
    if (!t.type_a) {
        return t.nouter + j;
    }
    if (j < t.m1) {
        size_t q1 = t.m1 / kGroup;
        return t.nouter + kGroup * (j % q1) + j / q1;
    }
    size_t q2 = (parity_length(t) - t.m1) / kGroup;
    size_t k = j - t.m1;
    return t.nouter + t.m1 + kGroup * (k % q2) + k / q2;
}

// Accumulators chained by the dual-diagonal (p_j ^= p_{j-1}) part
size_t dual_diagonal_length(const LdpcTable& t) {
    return t.type_a ? t.m1 : parity_length(t);
}

void add_edge(SparseMatrix& H, size_t row, size_t col) {
    H.row_indices[row].push_back(static_cast<uint16_t>(col));
    H.col_indices[col].push_back(static_cast<uint16_t>(row));
}

}  // namespace

SparseMatrix LdpcMatrixGenerator::generate(config::CodeRate rate, bool short_codeword) const {
    SparseMatrix H;
    const LdpcTable* t = table_for(rate, short_codeword);
    if (t == nullptr) {
        return H;
    }

    H.num_rows = parity_length(*t);
    H.num_cols = t->ninner;
    H.info_bits = t->nouter;
    H.row_indices.resize(H.num_rows);
    H.col_indices.resize(H.num_cols);

    // Table row r covers codeword bits 360r..360r+359: info bits for every
    // row, and for Type A also the M1 parity bits (step vii), which follow
    // the info bits in the codeword.
    for (size_t r = 0; r < t->num_rows; ++r) {
        for (size_t m = 0; m < kGroup; ++m) {
            for (size_t e = t->row_start[r]; e < t->row_start[r + 1]; ++e) {
                add_edge(H, accumulator(*t, t->addresses[e], m), kGroup * r + m);
            }
        }
    }

    // Check j closes accumulator j: p_j = a_j (+ p_{j-1} on the dual diagonal)
    for (size_t j = 0; j < H.num_rows; ++j) {
        add_edge(H, j, parity_column(*t, j));
        if (j > 0 && j < dual_diagonal_length(*t)) {
            add_edge(H, j, parity_column(*t, j - 1));
        }
    }

    for (auto& row : H.row_indices) {
        std::sort(row.begin(), row.end());
    }
    for (auto& col : H.col_indices) {
        std::sort(col.begin(), col.end());
    }
    return H;
}

bool ldpc_encode(config::CodeRate rate, bool short_codeword, const uint8_t* info,
                 uint8_t* codeword) {
    const LdpcTable* t = table_for(rate, short_codeword);
    if (t == nullptr) {
        return false;
    }
    const size_t minner = parity_length(*t);
    std::vector<uint8_t> p(minner, 0);

    auto accumulate_group = [&](size_t row, const uint8_t* bits) {
        for (size_t m = 0; m < kGroup; ++m) {
            if (bits[m] & 1) {
                for (size_t e = t->row_start[row]; e < t->row_start[row + 1]; ++e) {
                    p[accumulator(*t, t->addresses[e], m)] ^= 1;
                }
            }
        }
    };

    std::copy(info, info + t->nouter, codeword);
    const size_t info_groups = t->nouter / kGroup;
    for (size_t g = 0; g < info_groups; ++g) {
        accumulate_group(g, info + kGroup * g);
    }
    for (size_t j = 1; j < dual_diagonal_length(*t); ++j) {
        p[j] ^= p[j - 1];
    }

    if (!t->type_a) {
        std::copy(p.begin(), p.end(), codeword + t->nouter);
        return true;
    }

    // Type A: interleave the M1 part (step vi), let it feed the M2
    // accumulators (step vii), then interleave the M2 part (step viii).
    const size_t q1 = t->m1 / kGroup;
    const size_t q2 = (minner - t->m1) / kGroup;
    for (size_t s = 0; s < kGroup; ++s) {
        for (size_t u = 0; u < q1; ++u) {
            codeword[t->nouter + kGroup * u + s] = p[q1 * s + u];
        }
    }
    for (size_t g = 0; g < q1; ++g) {
        accumulate_group(info_groups + g, codeword + t->nouter + kGroup * g);
    }
    for (size_t s = 0; s < kGroup; ++s) {
        for (size_t u = 0; u < q2; ++u) {
            codeword[t->nouter + t->m1 + kGroup * u + s] = p[t->m1 + q2 * s + u];
        }
    }
    return true;
}

bool LdpcMatrixGenerator::validate_matrix(const SparseMatrix& H, config::CodeRate rate,
                                          bool short_codeword) const {
    size_t expected_n = short_codeword ? 16200 : 64800;
    size_t expected_k = get_ldpc_info_bits(short_codeword, rate);
    size_t expected_m = expected_n - expected_k;

    if (H.num_cols != expected_n) {
        return false;
    }
    if (H.num_rows != expected_m) {
        return false;
    }
    if (H.info_bits != expected_k) {
        return false;
    }

    // Check that row and column indices are properly sized
    if (H.row_indices.size() != expected_m) {
        return false;
    }
    if (H.col_indices.size() != expected_n) {
        return false;
    }

    // Check minimum connectivity (each row should have some connections)
    for (const auto& row : H.row_indices) {
        if (row.empty()) {
            return false;  // Disconnected check node
        }
    }

    return true;
}

std::string get_ldpc_matrix_path(config::CodeRate rate, bool short_codeword) {
    static const char* rate_names[] = {"2_15", "3_15", "4_15",  "5_15",  "6_15",  "7_15",
                                       "8_15", "9_15", "10_15", "11_15", "12_15", "13_15"};

    size_t rate_idx = static_cast<size_t>(rate);
    if (rate_idx >= 12) {
        return "";
    }

    const char* length = short_codeword ? "short" : "long";

    // Return path relative to config directory
    return std::string("config/ldpc_tables/ldpc_") + length + "_" + rate_names[rate_idx] + ".json";
}

SparseMatrix load_ldpc_matrix(const std::string& filepath) {
    SparseMatrix H;

    std::ifstream file(filepath);
    if (!file.is_open()) {
        return H;  // Return empty matrix on failure
    }

    // Simple JSON parsing for our specific format
    // Format: {"num_rows":N, "num_cols":M, "info_bits":K, "row_indices":[[...], ...]}
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // Basic parsing - look for key numeric values
    // (Full JSON parsing would require a library)

    // For now, return empty - actual loading would parse JSON
    // The generator creates matrices on-demand, so loading is optional

    return H;
}

bool save_ldpc_matrix(const SparseMatrix& H, const std::string& filepath) {
    if (!H.is_valid()) {
        return false;
    }

    std::ofstream file(filepath);
    if (!file.is_open()) {
        return false;
    }

    // Write JSON format
    file << "{\n";
    file << "  \"num_rows\": " << H.num_rows << ",\n";
    file << "  \"num_cols\": " << H.num_cols << ",\n";
    file << "  \"info_bits\": " << H.info_bits << ",\n";

    // Write row indices
    file << "  \"row_indices\": [\n";
    for (size_t i = 0; i < H.row_indices.size(); ++i) {
        file << "    [";
        const auto& row = H.row_indices[i];
        for (size_t j = 0; j < row.size(); ++j) {
            file << row[j];
            if (j < row.size() - 1)
                file << ", ";
        }
        file << "]";
        if (i < H.row_indices.size() - 1)
            file << ",";
        file << "\n";
    }
    file << "  ]\n";
    file << "}\n";

    return true;
}

}  // namespace fec
}  // namespace atsc3
