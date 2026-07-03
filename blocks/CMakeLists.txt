# blocks/ - GNU Radio OOT blocks
# Thin wrappers that delegate immediately to lib/. No DSP logic here.

if(NOT Gnuradio_FOUND)
    message(STATUS "Skipping blocks/ - GNU Radio not found")
    return()
endif()

set(BLOCKS_SOURCES
    atsc3_bootstrap_detect_impl.cc
    atsc3_ofdm_demod_impl.cc
    atsc3_channel_eq_impl.cc
    atsc3_constellation_demapper_impl.cc
    atsc3_cell_deinterleaver_impl.cc
    atsc3_freq_deinterleaver_impl.cc
    atsc3_time_deinterleaver_impl.cc
    atsc3_fec_decode_impl.cc
    atsc3_alp_demux_impl.cc
)

set(BLOCKS_HEADERS
    atsc3_bootstrap_detect.h
    atsc3_ofdm_demod.h
    atsc3_channel_eq.h
    atsc3_constellation_demapper.h
    atsc3_cell_deinterleaver.h
    atsc3_freq_deinterleaver.h
    atsc3_time_deinterleaver.h
    atsc3_fec_decode.h
    atsc3_alp_demux.h
)

if(BLOCKS_SOURCES)
    add_library(gnuradio-atsc3 SHARED ${BLOCKS_SOURCES})

    target_include_directories(gnuradio-atsc3
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/lib>
            $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(gnuradio-atsc3
        PRIVATE
            atsc3_lib
            atsc3_hal
            gnuradio::gnuradio-runtime
            gnuradio::gnuradio-blocks
    )

    set_target_properties(gnuradio-atsc3 PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )

    install(TARGETS gnuradio-atsc3
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
    )

    # Install public headers
    install(FILES ${BLOCKS_HEADERS}
        DESTINATION include/gnuradio/atsc3
    )

    # Install GRC block definitions
    install(FILES
        grc/atsc3_bootstrap_detect.block.yml
        grc/atsc3_ofdm_demod.block.yml
        grc/atsc3_channel_eq.block.yml
        grc/atsc3_constellation_demapper.block.yml
        grc/atsc3_cell_deinterleaver.block.yml
        grc/atsc3_freq_deinterleaver.block.yml
        grc/atsc3_time_deinterleaver.block.yml
        grc/atsc3_fec_decode.block.yml
        grc/atsc3_alp_demux.block.yml
        DESTINATION share/gnuradio/grc/blocks
    )
endif()
