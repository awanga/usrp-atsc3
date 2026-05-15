# lib/framing/ - Frame processing
# L1 preamble parsing, de-interleaving, ALP demux, ROUTE parsing

set(FRAMING_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/l1_decoder.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/alp_demux.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/service_catalog.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/route_parser.cc
    PARENT_SCOPE
)

set(FRAMING_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/l1_decoder.h       # AXI4-S: TDATA=int8(LLR) TVALID TREADY TLAST
    ${CMAKE_CURRENT_SOURCE_DIR}/alp_demux.h        # AXI4-S: TDATA=uint8 TVALID TREADY TLAST
    ${CMAKE_CURRENT_SOURCE_DIR}/service_catalog.h  # Service discovery data structures
    ${CMAKE_CURRENT_SOURCE_DIR}/route_parser.h     # ROUTE/DASH signaling parser
    PARENT_SCOPE
)
