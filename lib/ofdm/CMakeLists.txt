# lib/ofdm/ - OFDM processing
# FFT engine, CP removal, subcarrier mapping

set(OFDM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/fft_engine.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/cp_removal.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/pilot_extractor.cc
    # subcarrier_mapper.cc
    PARENT_SCOPE
)

set(OFDM_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/fft_engine.h       # AXI4-S: TDATA=cf32 TVALID TREADY TLAST(symbol)
    ${CMAKE_CURRENT_SOURCE_DIR}/cp_removal.h
    ${CMAKE_CURRENT_SOURCE_DIR}/pilot_extractor.h  # AXI4-S: TDATA=cf32 Output: Vector<PilotSymbol>
    # subcarrier_mapper.h
    PARENT_SCOPE
)
