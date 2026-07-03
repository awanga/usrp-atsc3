# python/bindings/ - pybind11 module for gr-atsc3

set(BINDING_SOURCES
    python_bindings.cc
    bootstrap_detect_python.cc
    ofdm_demod_python.cc
    channel_eq_python.cc
    constellation_demapper_python.cc
    cell_deinterleaver_python.cc
    freq_deinterleaver_python.cc
    time_deinterleaver_python.cc
    fec_decode_python.cc
    alp_demux_python.cc
)

pybind11_add_module(atsc3_python ${BINDING_SOURCES})

target_include_directories(atsc3_python
    PRIVATE
        ${CMAKE_SOURCE_DIR}/blocks
        ${CMAKE_SOURCE_DIR}/lib
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(atsc3_python
    PRIVATE
        gnuradio-atsc3
        gnuradio::gnuradio-runtime
        gnuradio::gnuradio-blocks
)

set_target_properties(atsc3_python PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "atsc3_python"
)

# Install the compiled module to the gnuradio.atsc3 package directory
install(TARGETS atsc3_python
    LIBRARY DESTINATION ${PYTHON_INSTALL_DIR}/gnuradio/atsc3
)
