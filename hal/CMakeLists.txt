# hal/ - Hardware Abstraction Layer
# IQSource interface + UHDSource / FileSource / NullSource implementations

set(HAL_SOURCES
    # Source files will be added here
    # src/iq_source.cc
    # src/uhd_source.cc
    # src/file_source.cc
    # src/null_source.cc
)

set(HAL_HEADERS
    # Header files will be added here
    # include/iq_source.h
)

if(HAL_SOURCES)
    add_library(atsc3_hal ${HAL_SOURCES})

    target_include_directories(atsc3_hal
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(atsc3_hal
        PRIVATE
            atsc3_lib
    )

    if(UHD_FOUND)
        target_link_libraries(atsc3_hal PRIVATE ${UHD_LIBRARIES})
        target_include_directories(atsc3_hal PRIVATE ${UHD_INCLUDE_DIRS})
        target_compile_definitions(atsc3_hal PRIVATE ATSC3_HAS_UHD=1)
    endif()
endif()
