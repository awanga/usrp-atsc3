# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Alfred Wanga
#
# lib/dsp/ - Shared fixed-point DSP primitives (RTL-portable)
# CORDIC core: no multiplier, shift-add + table lookup only.
# Resampler: rational polyphase sample-rate conversion.

set(DSP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/cordic.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/resampler.cc
    PARENT_SCOPE
)

set(DSP_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/cordic.h
    ${CMAKE_CURRENT_SOURCE_DIR}/resampler.h
    PARENT_SCOPE
)
