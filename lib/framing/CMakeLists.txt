# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Alfred Wanga
#
# lib/framing/ - Frame processing
# L1 preamble parsing, de-interleaving, ALP demux, ROUTE parsing

set(FRAMING_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/l1_fec.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/l1_signaling.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/preamble_demod.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/baseband_packet.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/alp_demux.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/service_catalog.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/route_parser.cc
    PARENT_SCOPE
)

set(FRAMING_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/l1_fec.h           # AXI4-S: TDATA=int8(LLR) TVALID TREADY TLAST(coded block)
    ${CMAKE_CURRENT_SOURCE_DIR}/l1_signaling.h     # L1-Basic / L1-Detail field parsing
    ${CMAKE_CURRENT_SOURCE_DIR}/preamble_demod.h
    ${CMAKE_CURRENT_SOURCE_DIR}/baseband_packet.h  # AXI4-S: TDATA=uint8 TVALID TREADY TLAST(BBP)
    ${CMAKE_CURRENT_SOURCE_DIR}/alp_demux.h        # AXI4-S: TDATA=uint8 TVALID TREADY TLAST
    ${CMAKE_CURRENT_SOURCE_DIR}/service_catalog.h  # Service discovery data structures
    ${CMAKE_CURRENT_SOURCE_DIR}/route_parser.h     # ROUTE/DASH signaling parser
    PARENT_SCOPE
)
