# lib/ - Core DSP Library
# Pure C++17 + FFTW3 + Boost only. No GR/UHD headers allowed.

# Collect sources from subdirectories
set(LIB_SOURCES
    # Sources will be added as modules are implemented
)

set(LIB_HEADERS
    types.h
)

# Add subdirectory CMake files for organization
add_subdirectory(sync)
add_subdirectory(ofdm)
add_subdirectory(channel)
add_subdirectory(fec)
add_subdirectory(framing)
add_subdirectory(metrics)

# Create header-only interface library for now
# Will become a real library once sources are added
add_library(atsc3_lib INTERFACE)

target_include_directories(atsc3_lib
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include/atsc3>
)

target_link_libraries(atsc3_lib
    INTERFACE
        ${FFTW3_LIBRARIES}
        Boost::system
)

target_compile_definitions(atsc3_lib
    INTERFACE
        $<$<BOOL:${ATSC3_FIXED_POINT}>:ATSC3_FIXED_POINT=1>
)

# When sources are added, switch to:
# add_library(atsc3_lib STATIC ${LIB_SOURCES})
