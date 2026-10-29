# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Alfred Wanga
#
# lib/dsp/ - Shared fixed-point DSP primitives (RTL-portable)
# CORDIC core: no multiplier, shift-add + table lookup only.

set(DSP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/cordic.cc
    PARENT_SCOPE
)

set(DSP_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/cordic.h
    PARENT_SCOPE
)
