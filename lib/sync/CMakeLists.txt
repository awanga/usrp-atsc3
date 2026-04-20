# lib/sync/ - Synchronization primitives
# Bootstrap detection, frame sync, timing recovery, frequency correction

set(SYNC_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/bootstrap_detector.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/freq_correction.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/frame_sync.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/timing_recovery.cc
    PARENT_SCOPE
)

set(SYNC_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/bootstrap_detector.h
    ${CMAKE_CURRENT_SOURCE_DIR}/freq_correction.h
    ${CMAKE_CURRENT_SOURCE_DIR}/frame_sync.h
    ${CMAKE_CURRENT_SOURCE_DIR}/timing_recovery.h
    PARENT_SCOPE
)
