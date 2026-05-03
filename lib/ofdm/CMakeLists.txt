# lib/ofdm/ - OFDM processing
# FFT engine, CP removal, subcarrier mapping

set(OFDM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/fft_engine.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/cp_removal.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/pilot_extractor.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/constellation_demapper.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/cell_deinterleaver.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/time_deinterleaver.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/freq_deinterleaver.cc
    # subcarrier_mapper.cc
    PARENT_SCOPE
)

set(OFDM_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/fft_engine.h       # AXI4-S: TDATA=cf32 TVALID TREADY TLAST(symbol)
    ${CMAKE_CURRENT_SOURCE_DIR}/cp_removal.h
    ${CMAKE_CURRENT_SOURCE_DIR}/pilot_extractor.h  # AXI4-S: TDATA=cf32 Output: Vector<PilotSymbol>
    ${CMAKE_CURRENT_SOURCE_DIR}/constellation_demapper.h  # AXI4-S: TDATA=int8(LLR) TVALID TREADY TLAST
    ${CMAKE_CURRENT_SOURCE_DIR}/cell_deinterleaver.h  # AXI4-S: TDATA=int8 TVALID TREADY TLAST(FEC block)
    ${CMAKE_CURRENT_SOURCE_DIR}/time_deinterleaver.h  # AXI4-S: TDATA=int8 TVALID TREADY TLAST(TI block)
    ${CMAKE_CURRENT_SOURCE_DIR}/freq_deinterleaver.h  # AXI4-S: TDATA=int8 TVALID TREADY TLAST(symbol)
    # subcarrier_mapper.h
    PARENT_SCOPE
)
