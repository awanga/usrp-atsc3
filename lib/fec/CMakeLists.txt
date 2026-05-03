# lib/fec/ - Forward Error Correction
# LDPC decoder, BCH decoder, LLR demapper

set(FEC_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ldpc_decoder.cc
    # bch_decoder.cc
    # llr_demapper.cc
    PARENT_SCOPE
)

set(FEC_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/ldpc_decoder.h  # AXI4-S: TDATA=int8(LLR) TVALID TREADY TLAST(codeword)
    # bch_decoder.h
    # llr_demapper.h
    PARENT_SCOPE
)
