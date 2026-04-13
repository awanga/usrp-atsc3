# blocks/ - GNU Radio OOT blocks
# Thin wrappers that delegate immediately to lib/. No DSP logic here.

if(NOT Gnuradio_FOUND)
    message(STATUS "Skipping blocks/ - GNU Radio not found")
    return()
endif()

set(BLOCKS_SOURCES
    # atsc3_source_impl.cc
    # atsc3_demod_impl.cc
    # atsc3_decoder_impl.cc
)

set(BLOCKS_HEADERS
    # atsc3_source.h
    # atsc3_demod.h
    # atsc3_decoder.h
)

if(BLOCKS_SOURCES)
    add_library(gnuradio-atsc3 SHARED ${BLOCKS_SOURCES})

    target_include_directories(gnuradio-atsc3
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(gnuradio-atsc3
        PRIVATE
            atsc3_lib
            atsc3_hal
            gnuradio::gnuradio-runtime
            gnuradio::gnuradio-blocks
    )

    install(TARGETS gnuradio-atsc3
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
    )
endif()
