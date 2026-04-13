# lib/channel/ - Channel estimation and equalization
# LS estimation, Wiener filtering, FDE equalizer

set(CHANNEL_SOURCES
    # channel_estimator.cc
    # wiener_filter.cc
    # fde_equalizer.cc
)

set(CHANNEL_HEADERS
    # channel_estimator.h  -- has setBackend(EstimatorBackend) slot for ML (post-MVP)
    # wiener_filter.h
    # fde_equalizer.h
)

# Sources will be added to parent LIB_SOURCES when implemented
