cmake_minimum_required(VERSION 3.16)

project(
  gr-atsc3
  VERSION 1.0.0
  DESCRIPTION "GNU Radio OOT module for ATSC 3.0 physical-layer receiver"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Append custom CMake module path
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/Modules)

# ------------------------------------------------------------------------------
# Build Options
# ------------------------------------------------------------------------------
option(ATSC3_FIXED_POINT "Build with fixed-point (int16_t) sample type" OFF)
option(ATSC3_ENABLE_ML "Enable ML inference backend (post-MVP)" OFF)
option(ATSC3_ENABLE_HDL_STUBS "Build HDL stub generation targets (post-MVP)"
       OFF)
option(ATSC3_BUILD_TESTS "Build test suite" ON)
option(ATSC3_BUILD_FUZZ "Build libFuzzer harnesses (requires clang)" OFF)

set(ATSC3_GR_VERSION
    "3.10"
    CACHE STRING "Target GNU Radio version (3.8, 3.9, 3.10)")
set(ATSC3_CHANNEL_BW_HZ
    "6000000"
    CACHE STRING "Channel bandwidth in Hz (6 MHz default)")

# SIMD tier selection SCALAR: No SIMD, portable C++ only SSSE3:  128-bit vectors
# (SSE2 + SSSE3 + SSE4.1 + SSE4.2) AVX2:   256-bit vectors (all SSE + AVX +
# AVX2) NATIVE: Auto-detect based on build host (uses -march=native)
set(ATSC3_SIMD_TIER
    "SSSE3"
    CACHE STRING "SIMD instruction set tier (SCALAR, SSSE3, AVX2, NATIVE)")

# ------------------------------------------------------------------------------
# Compiler Flags
# ------------------------------------------------------------------------------
add_compile_options(-Wall -Wextra -Werror -Wshadow -fstack-usage
                    $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>)

# -Wdouble-promotion is useful but conflicts with GoogleTest Apply it only to
# our source directories via target properties

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-g -O0)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O3 -DNDEBUG)
endif()

# ------------------------------------------------------------------------------
# SIMD Configuration
# ------------------------------------------------------------------------------
# Validate and set SIMD tier flags
if(ATSC3_SIMD_TIER STREQUAL "SCALAR")
  message(STATUS "SIMD disabled (scalar mode)")
  # No SIMD flags
elseif(ATSC3_SIMD_TIER STREQUAL "SSSE3")
  message(STATUS "SIMD tier: SSSE3 (128-bit vectors)")
  add_compile_options(-mssse3)
  add_compile_definitions(ATSC3_SIMD_SSSE3=1)
elseif(ATSC3_SIMD_TIER STREQUAL "AVX2")
  message(STATUS "SIMD tier: AVX2 (256-bit vectors)")
  add_compile_options(-mavx2 -mfma)
  add_compile_definitions(ATSC3_SIMD_AVX2=1 ATSC3_SIMD_SSSE3=1)
elseif(ATSC3_SIMD_TIER STREQUAL "NATIVE")
  message(STATUS "SIMD tier: NATIVE (auto-detect)")
  add_compile_options(-march=native)
  # Runtime detection will determine available features
  add_compile_definitions(ATSC3_SIMD_NATIVE=1)
else()
  message(
    WARNING "Unknown SIMD tier '${ATSC3_SIMD_TIER}', defaulting to SCALAR")
  set(ATSC3_SIMD_TIER
      "SCALAR"
      CACHE STRING "SIMD tier" FORCE)
endif()

# ------------------------------------------------------------------------------
# Fixed-point configuration
# ------------------------------------------------------------------------------
if(ATSC3_FIXED_POINT)
  add_compile_definitions(ATSC3_FIXED_POINT=1)
  message(STATUS "Building with fixed-point sample type (int16_t)")
else()
  message(STATUS "Building with floating-point sample type (float)")
endif()

# ------------------------------------------------------------------------------
# Find Dependencies
# ------------------------------------------------------------------------------

# FFTW3 (required for OFDM processing)
find_package(FFTW3)
if(NOT FFTW3_FOUND)
  message(
    WARNING "FFTW3 not found - DSP library will have limited functionality")
  set(FFTW3_LIBRARIES "")
  set(FFTW3_INCLUDE_DIRS "")
endif()

# Boost (required for various utilities)
find_package(Boost 1.71 REQUIRED COMPONENTS system)

# GNU Radio (required for blocks/)
find_package(Gnuradio ${ATSC3_GR_VERSION} COMPONENTS runtime blocks fft filter)
if(Gnuradio_FOUND)
  message(STATUS "Found GNU Radio ${Gnuradio_VERSION}")
else()
  message(WARNING "GNU Radio not found - blocks/ will not be built")
endif()

# UHD (required for hal/)
find_package(UHD)
if(UHD_FOUND)
  message(STATUS "Found UHD ${UHD_VERSION}")
else()
  message(WARNING "UHD not found - hardware sources will be disabled")
endif()

# Python and pybind11 (required for Python bindings)
if(Gnuradio_FOUND)
  find_package(
    Python3
    COMPONENTS Interpreter Development
    REQUIRED)
  find_package(pybind11 CONFIG)
  if(pybind11_FOUND)
    message(STATUS "Found pybind11 ${pybind11_VERSION}")
    # Determine Python site-packages installation directory
    execute_process(
      COMMAND ${Python3_EXECUTABLE} -c
              "import sysconfig; print(sysconfig.get_path('platlib'))"
      OUTPUT_VARIABLE PYTHON_SITE_PACKAGES
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(PYTHON_INSTALL_DIR
        ${PYTHON_SITE_PACKAGES}
        CACHE PATH "Python site-packages installation directory")
    message(STATUS "Python install dir: ${PYTHON_INSTALL_DIR}")
  else()
    message(WARNING "pybind11 not found - Python bindings will not be built")
  endif()
endif()

# ------------------------------------------------------------------------------
# GoogleTest (for testing)
# ------------------------------------------------------------------------------
if(ATSC3_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0)
  # Prevent overriding the parent project's compiler/linker settings
  set(gtest_force_shared_crt
      ON
      CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  include(GoogleTest)
endif()

# ------------------------------------------------------------------------------
# Subdirectories
# ------------------------------------------------------------------------------

# Core DSP library (no GR/UHD dependencies)
add_subdirectory(lib)

# Hardware abstraction layer
add_subdirectory(hal)

# Audio/Video processing (must be before blocks/ so atsc3_av target exists)
add_subdirectory(av)

# GNU Radio blocks (only if GR found)
if(Gnuradio_FOUND)
  add_subdirectory(blocks)
endif()

# Python bindings (only if GR and pybind11 found)
if(Gnuradio_FOUND AND pybind11_FOUND)
  add_subdirectory(python)
endif()

# Applications
add_subdirectory(apps)

# HDL stubs (post-MVP)
if(ATSC3_ENABLE_HDL_STUBS)
  add_subdirectory(hdl)
endif()

# ML backend (post-MVP)
if(ATSC3_ENABLE_ML)
  add_subdirectory(ml)
endif()

# Tests
if(ATSC3_BUILD_TESTS)
  add_subdirectory(test)
endif()

# ------------------------------------------------------------------------------
# CPack Configuration for .deb packaging
# ------------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "gr-atsc3")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY ${PROJECT_DESCRIPTION})
set(CPACK_PACKAGE_MAINTAINER "ATSC3 Project")
set(CPACK_PACKAGE_CONTACT "atsc3@example.com")

set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libfftw3-dev, libboost-system-dev")
set(CPACK_DEBIAN_PACKAGE_SECTION "comm")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")

include(CPack)

# ------------------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------------------
message(STATUS "")
message(STATUS "gr-atsc3 Configuration Summary:")
message(STATUS "  Version:        ${PROJECT_VERSION}")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  Fixed-point:    ${ATSC3_FIXED_POINT}")
message(STATUS "  SIMD tier:      ${ATSC3_SIMD_TIER}")
message(STATUS "  GR version:     ${ATSC3_GR_VERSION}")
message(STATUS "  Channel BW:     ${ATSC3_CHANNEL_BW_HZ} Hz")
message(STATUS "  Python bindings:${pybind11_FOUND}")
message(STATUS "  Build tests:    ${ATSC3_BUILD_TESTS}")
message(STATUS "  ML backend:     ${ATSC3_ENABLE_ML}")
message(STATUS "  HDL stubs:      ${ATSC3_ENABLE_HDL_STUBS}")
message(STATUS "")
