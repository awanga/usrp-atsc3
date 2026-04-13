# lib/ofdm/ - OFDM processing
# FFT engine, CP removal, subcarrier mapping

set(OFDM_SOURCES
    # fft_engine.cc
    # cp_removal.cc
    # subcarrier_mapper.cc
)

set(OFDM_HEADERS
    # fft_engine.h       -- AXI4-S: TDATA=cf32 TVALID TREADY TLAST(symbol)
    # cp_removal.h
    # subcarrier_mapper.h
)

# Sources will be added to parent LIB_SOURCES when implemented
