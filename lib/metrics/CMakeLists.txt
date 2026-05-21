# lib/metrics/ - Signal quality metrics
# SNR estimation, MER calculation, BER/FER tracking, signal strength

set(METRICS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/snr_estimator.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/mer_estimator.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/ber_estimator.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/signal_strength.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/metrics_aggregator.cc
    PARENT_SCOPE
)

set(METRICS_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/snr_estimator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/mer_estimator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ber_estimator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/signal_strength.h
    ${CMAKE_CURRENT_SOURCE_DIR}/metrics_aggregator.h
    PARENT_SCOPE
)
