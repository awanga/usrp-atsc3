# lib/simd/CMakeLists.txt — SIMD support library
#
# Provides CPU feature detection and SIMD-optimized kernels.
# Compiles separate object files for each SIMD tier to allow
# runtime dispatch to the best available implementation.

set(SIMD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/cpu_features.cc
)

set(SIMD_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/cpu_features.h
    ${CMAKE_CURRENT_SOURCE_DIR}/simd_types.h
)

# Export sources to parent
set(SIMD_SOURCES ${SIMD_SOURCES} PARENT_SCOPE)
set(SIMD_HEADERS ${SIMD_HEADERS} PARENT_SCOPE)

# SIMD tier-specific compilation
# Each tier gets its own object file with appropriate compiler flags

# SSSE3 tier (128-bit vectors)
if(ATSC3_SIMD_TIER STREQUAL "SSSE3" OR ATSC3_SIMD_TIER STREQUAL "AVX2" OR ATSC3_SIMD_TIER STREQUAL "NATIVE")
    # SSSE3 kernels compiled with SSSE3 flags
    # These are always available when SIMD is enabled
endif()

# AVX2 tier (256-bit vectors)
if(ATSC3_SIMD_TIER STREQUAL "AVX2" OR ATSC3_SIMD_TIER STREQUAL "NATIVE")
    # AVX2 kernels compiled with AVX2 flags
    # Runtime dispatch checks CPU support before calling
endif()

# Print SIMD configuration
message(STATUS "SIMD tier: ${ATSC3_SIMD_TIER}")
