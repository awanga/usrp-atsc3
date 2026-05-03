# lib/framing/ - Frame processing
# L1 preamble parsing, de-interleaving, ALP demux

set(FRAMING_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/l1_decoder.cc
    # alp_demux.cc
    PARENT_SCOPE
)

set(FRAMING_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/l1_decoder.h  # AXI4-S: TDATA=int8(LLR) TVALID TREADY TLAST
    # alp_demux.h
    PARENT_SCOPE
)
