cmake_minimum_required(VERSION 3.16)

project(gr-atsc3
    VERSION 0.1.0
    DESCRIPTION "GNU Radio OOT module for ATSC 3.0 physical-layer receiver"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Append custom CMake module path
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/Modules)

#------------------------------------------------------------------------------
# Build Options
#------------------------------------------------------------------------------
option(ATSC3_FIXED_POINT "Build with fixed-point (int16_t) sample type" OFF)
option(ATSC3_ENABLE_ML "Enable ML inference backend (post-MVP)" OFF)
option(ATSC3_ENABLE_HDL_STUBS "Build HDL stub generation targets (post-MVP)" OFF)
option(ATSC3_BUILD_TESTS "Build test suite" ON)

set(ATSC3_GR_VERSION "3.10" CACHE STRING "Target GNU Radio version (3.8, 3.9, 3.10)")
set(ATSC3_CHANNEL_BW_HZ "6000000" CACHE STRING "Channel bandwidth in Hz (6 MHz default)")

#------------------------------------------------------------------------------
# Compiler Flags
#------------------------------------------------------------------------------
add_compile_options(
    -Wall
    -Wextra
    -Werror
    -Wshadow
    -Wdouble-promotion
    -fstack-usage
    $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -DNDEBUG)
endif()

#------------------------------------------------------------------------------
# Fixed-point configuration
#------------------------------------------------------------------------------
if(ATSC3_FIXED_POINT)
    add_compile_definitions(ATSC3_FIXED_POINT=1)
    message(STATUS "Building with fixed-point sample type (int16_t)")
else()
    message(STATUS "Building with floating-point sample type (float)")
endif()

#------------------------------------------------------------------------------
# Find Dependencies
#------------------------------------------------------------------------------

# FFTW3 (required for OFDM processing)
find_package(FFTW3)
if(NOT FFTW3_FOUND)
    message(WARNING "FFTW3 not found - DSP library will have limited functionality")
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

#------------------------------------------------------------------------------
# GoogleTest (for testing)
#------------------------------------------------------------------------------
if(ATSC3_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    # Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    include(GoogleTest)
endif()

#------------------------------------------------------------------------------
# Subdirectories
#------------------------------------------------------------------------------

# Core DSP library (no GR/UHD dependencies)
add_subdirectory(lib)

# Hardware abstraction layer
add_subdirectory(hal)

# GNU Radio blocks (only if GR found)
if(Gnuradio_FOUND)
    add_subdirectory(blocks)
endif()

# Applications
add_subdirectory(apps)

# Audio/Video processing
add_subdirectory(av)

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

#------------------------------------------------------------------------------
# CPack Configuration for .deb packaging
#------------------------------------------------------------------------------
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

#------------------------------------------------------------------------------
# Summary
#------------------------------------------------------------------------------
message(STATUS "")
message(STATUS "gr-atsc3 Configuration Summary:")
message(STATUS "  Version:        ${PROJECT_VERSION}")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  Fixed-point:    ${ATSC3_FIXED_POINT}")
message(STATUS "  GR version:     ${ATSC3_GR_VERSION}")
message(STATUS "  Channel BW:     ${ATSC3_CHANNEL_BW_HZ} Hz")
message(STATUS "  Build tests:    ${ATSC3_BUILD_TESTS}")
message(STATUS "  ML backend:     ${ATSC3_ENABLE_ML}")
message(STATUS "  HDL stubs:      ${ATSC3_ENABLE_HDL_STUBS}")
message(STATUS "")
