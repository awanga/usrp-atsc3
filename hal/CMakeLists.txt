# hal/ - Hardware Abstraction Layer
# IQSource interface + UHDSource / FileSource / NullSource implementations

set(HAL_SOURCES
    src/iq_source.cc
    src/null_source.cc
    src/file_source.cc
    src/uhd_source.cc
    src/agc.cc
)

set(HAL_HEADERS
    include/iq_source.h
    include/iq_source_factory.h
    include/agc.h
)

add_library(atsc3_hal ${HAL_SOURCES})
set_target_properties(atsc3_hal PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(atsc3_hal
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# HAL has no dependency on lib/ - it's a lower layer
# lib/ may depend on hal/ but not vice versa

if(UHD_FOUND)
    target_link_libraries(atsc3_hal PRIVATE ${UHD_LIBRARIES})
    target_include_directories(atsc3_hal PRIVATE ${UHD_INCLUDE_DIRS})
    target_compile_definitions(atsc3_hal PRIVATE ATSC3_HAS_UHD=1)
endif()
