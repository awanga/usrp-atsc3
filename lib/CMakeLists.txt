# lib/ - Core DSP Library
# Pure C++17 + FFTW3 + Boost only. No GR/UHD headers allowed.

# Add subdirectory CMake files (sets SYNC_SOURCES, OFDM_SOURCES, etc.)
add_subdirectory(sync)
add_subdirectory(ofdm)
add_subdirectory(channel)
add_subdirectory(fec)
add_subdirectory(framing)
add_subdirectory(metrics)

# Collect all sources
set(LIB_SOURCES
    ${SYNC_SOURCES}
    ${OFDM_SOURCES}
    # ${CHANNEL_SOURCES}  -- Phase 3
    # ${FEC_SOURCES}
    # ${FRAMING_SOURCES}
    # ${METRICS_SOURCES}
)

set(LIB_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/types.h
    ${SYNC_HEADERS}
    ${OFDM_HEADERS}
    # ${CHANNEL_HEADERS}  -- Phase 3
)

# Create static library
add_library(atsc3_lib STATIC ${LIB_SOURCES})

target_include_directories(atsc3_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
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
