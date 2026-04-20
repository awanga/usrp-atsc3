# lib/channel/ - Channel estimation and equalization
# LS estimation, Wiener filtering, FDE equalizer

set(CHANNEL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/channel_estimator.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/wiener_interpolator.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/equalizer.cc
    PARENT_SCOPE
)

set(CHANNEL_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/channel_estimator.h  # has setBackend(EstimatorBackend) slot for ML (post-MVP)
    ${CMAKE_CURRENT_SOURCE_DIR}/wiener_interpolator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/equalizer.h
    PARENT_SCOPE
)
