# lib/ - Core DSP Library
# Pure C++17 + FFTW3 + Boost only. No GR/UHD headers allowed.

# Add subdirectory CMake files (sets SYNC_SOURCES, OFDM_SOURCES, etc.)
add_subdirectory(simd)
add_subdirectory(dsp)
add_subdirectory(sync)
add_subdirectory(ofdm)
add_subdirectory(channel)
add_subdirectory(fec)
add_subdirectory(framing)
add_subdirectory(metrics)

# Collect all sources
set(LIB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/config/config_bus.cc
    ${SIMD_SOURCES}
    ${DSP_SOURCES}
    ${SYNC_SOURCES}
    ${OFDM_SOURCES}
    ${CHANNEL_SOURCES}
    ${FEC_SOURCES}
    ${FRAMING_SOURCES}
    ${METRICS_SOURCES}
)

set(LIB_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/types.h
    ${SIMD_HEADERS}
    ${DSP_HEADERS}
    ${SYNC_HEADERS}
    ${OFDM_HEADERS}
    ${CHANNEL_HEADERS}
    ${FEC_HEADERS}
)

# Create static library (with PIC for use in shared libraries like gnuradio-atsc3)
add_library(atsc3_lib STATIC ${LIB_SOURCES})
set_target_properties(atsc3_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(atsc3_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/config>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/simd>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/dsp>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sync>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ofdm>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/channel>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/fec>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/framing>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/metrics>
        $<INSTALL_INTERFACE:include/atsc3>
    PRIVATE
        $<$<NOT:$<BOOL:${ATSC3_FIXED_POINT}>>:${FFTW3_INCLUDE_DIRS}>
)

target_link_libraries(atsc3_lib
    PUBLIC
        Boost::system
    PRIVATE
        $<$<NOT:$<BOOL:${ATSC3_FIXED_POINT}>>:${FFTW3_LIBRARIES}>
)

target_compile_definitions(atsc3_lib
    PUBLIC
        $<$<BOOL:${ATSC3_FIXED_POINT}>:ATSC3_FIXED_POINT=1>
)

target_compile_features(atsc3_lib PUBLIC cxx_std_17)
