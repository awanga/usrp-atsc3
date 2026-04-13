# lib/fec/ - Forward Error Correction
# LDPC decoder, BCH decoder, LLR demapper

set(FEC_SOURCES
    # ldpc_decoder.cc
    # bch_decoder.cc
    # llr_demapper.cc
)

set(FEC_HEADERS
    # ldpc_decoder.h    -- AXI4-S: TDATA=int8(LLR) TVALID TREADY TLAST(codeword)
    # bch_decoder.h
    # llr_demapper.h
)

# Sources will be added to parent LIB_SOURCES when implemented
