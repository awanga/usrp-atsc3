# LDPC Min-Sum Vectorization Analysis

## Executive Summary

The current LDPC decoder has partial SIMD optimization for helper functions but the core min-sum iteration loop remains largely scalar. The primary bottleneck is **O(d²) edge index lookups** per iteration, not the arithmetic operations themselves. Significant performance gains are achievable through algorithmic improvements before SIMD vectorization of the core loop.

## Current State

### Existing SIMD Optimizations

| Component | Status | SIMD Tier |
|-----------|--------|-----------|
| `find_two_min_and_sign()` | Vectorized | AVX2/SSE |
| `clamp_llr_array()` | Vectorized | AVX2/SSE |
| `compute_hard_decision()` | Vectorized | AVX2/SSE |
| `early_term_threshold` check | Vectorized | AVX2/SSE |
| `min_sum_iteration()` - check node | **Scalar** | N/A |
| `min_sum_iteration()` - variable node | **Scalar** | N/A |
| `check_syndrome()` | **Scalar** | N/A |

### Critical Bottleneck: Edge Index Lookups

In `min_sum_iteration()` (ldpc_decoder.cc:591-702), the code performs linear searches to find edge indices:

```cpp
// Line 616-621: For each edge in check node, find its index in variable node's edge list
const auto& rows_for_col = H_.col_indices[col];
size_t edge_idx = 0;
for (size_t j = 0; j < rows_for_col.size(); ++j) {
    if (rows_for_col[j] == row) {
        edge_idx = j;
        break;
    }
}
```

This pattern appears 4 times in the function. For a check node with degree `d_c` and variable nodes with average degree `d_v`:
- **Complexity per check node:** O(d_c × d_v)
- **Complexity per iteration:** O(m × d_c × d_v) where m = number of check nodes

For ATSC 3.0 long codewords (64800 bits, ~56000 parity checks), this dominates runtime.

## Optimization Opportunities

### Priority 1: Pre-compute Edge Index Mappings

**Impact:** HIGH | **Complexity:** LOW | **Memory:** +2 bytes per edge

Add bidirectional edge mapping tables at construction time:

```cpp
// New members in SparseMatrix
std::vector<std::vector<uint16_t>> row_to_col_edge_idx;  // row_to_col_edge_idx[row][i] = col's edge idx
std::vector<std::vector<uint16_t>> col_to_row_edge_idx;  // col_to_row_edge_idx[col][i] = row's edge idx
```

**Result:** O(1) edge lookups instead of O(d). Expected 2-4x speedup.

### Priority 2: Layered Decoding Schedule

**Impact:** HIGH | **Complexity:** MEDIUM | **Memory:** None

Replace flooding schedule (all check nodes, then all variable nodes) with layered/turbo schedule:

```cpp
// Current: Flooding
for (row : all_check_nodes) { check_update(row); }
for (col : all_variable_nodes) { variable_update(col); }

// Proposed: Layered
for (row : all_check_nodes) {
    check_update(row);
    // Immediately update connected variable nodes
    for (col : H_.row_indices[row]) {
        variable_update_partial(col, row);
    }
}
```

**Result:** Converges in ~50% fewer iterations. Combined with Priority 1 gives 4-8x speedup.

### Priority 3: Fixed-Point LLR Processing

**Impact:** MEDIUM | **Complexity:** MEDIUM | **Memory:** -2 bytes per LLR

Current: `float` LLRs (32-bit) → 8 values per AVX2 register
Proposed: `int8_t` LLRs (8-bit) → 32 values per AVX2 register

The input LLRs are already `int8_t`. Converting internal processing to fixed-point:
- Eliminates float→int8→float conversions at boundaries
- Enables 4x more parallelism per SIMD instruction
- Better cache utilization

**Challenges:**
- Requires careful saturation handling
- Min-sum scaling factor (0.75) needs fixed-point representation
- Must validate numerical equivalence with float version

### Priority 4: Vectorize Check Node Update

**Impact:** MEDIUM | **Complexity:** HIGH | **Memory:** None

For high-degree check nodes (degree > 8), vectorize across edges within a single node:

```cpp
// Vectorized two-minimum + sign product for degree-16 check node
__m256 v0 = _mm256_loadu_ps(&vn_messages[0]);
__m256 v1 = _mm256_loadu_ps(&vn_messages[8]);
// ... compute min1, min2, sign_prod using SIMD ...
```

**Challenges:**
- Variable node degrees (2-20) make vectorization irregular
- Requires padding/masking for non-power-of-2 degrees
- Gather/scatter for indirect message access

**Assessment:** The existing `find_two_min_and_sign()` helper already provides this, but it's not integrated into the main loop due to the edge mapping overhead.

### Priority 5: Vectorize Syndrome Check

**Impact:** LOW | **Complexity:** LOW | **Memory:** None

```cpp
// Current scalar XOR (ldpc_decoder.cc:745-758)
for (uint16_t col : cols) {
    parity ^= hard_decision_[col];
}

// Proposed: Process 32 bits at a time with gather + XOR
// (only beneficial for very high-degree nodes)
```

## Recommended Implementation Order

1. **Pre-compute edge mappings** (1-2 days)
   - Add mapping tables to SparseMatrix
   - Populate during matrix construction
   - Update min_sum_iteration() to use direct lookups
   - **Expected speedup: 2-4x**

2. **Layered decoding** (2-3 days)
   - Refactor min_sum_iteration() for row-by-row update
   - Add unit tests for convergence equivalence
   - **Expected speedup: additional 1.5-2x (cumulative 3-8x)**

3. **Fixed-point LLRs** (3-5 days)
   - Create parallel int8 implementation
   - Add equivalence tests against float version
   - Runtime dispatch based on SIMD tier
   - **Expected speedup: additional 1.5-2x (cumulative 5-16x)**

4. **Profile-guided optimization** (ongoing)
   - After Priority 1-3, profile to find remaining hotspots
   - Targeted SIMD optimization of specific bottlenecks

## Memory Layout Considerations

Current sparse matrix layout causes irregular memory access:

```
row_indices: [row0: [c0,c1,c2], row1: [c3,c4], row2: [c0,c5,c6,c7], ...]
col_indices: [col0: [r0,r2], col1: [r0], col2: [r0], ...]
```

For RTL portability, consider:
- **Padded regular layout:** Pad all rows/columns to max degree, use mask for valid entries
- **Block-wise processing:** Group nodes by degree for vectorized processing

## Conclusions

1. **Edge index lookups are the primary bottleneck**, not arithmetic operations
2. **Algorithmic improvements** (edge mapping, layered decoding) should precede SIMD vectorization of the core loop
3. **Fixed-point processing** enables better SIMD utilization and is required for RTL portability
4. **Current SIMD optimizations** in helper functions are good but underutilized due to surrounding scalar code

## Files Affected

- `lib/fec/ldpc_decoder.h` - Add edge mapping members to SparseMatrix
- `lib/fec/ldpc_decoder.cc` - Implement edge mapping, layered decoding
- `lib/fec/ldpc_matrix.cc` - Populate edge mappings during generation
- `test/unit/test_ldpc_decoder.cc` - Add convergence equivalence tests

## Metrics for Success

| Metric | Current | Target |
|--------|---------|--------|
| Iterations to converge (64800-bit, SNR=3dB) | ~15-20 | ~8-12 |
| Time per iteration (single thread) | TBD | 50% reduction |
| Time per codeword (single thread) | TBD | 4-8x reduction |
| Fixed-point equivalence | N/A | BER within 0.1dB |

---

*Analysis Date: 2026-07-16*
*Author: Claude Code*
