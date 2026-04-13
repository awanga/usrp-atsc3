# av/ - Audio/Video processing
# FFmpeg HEVC/AC-4 decode + GStreamer playback

set(AV_SOURCES
    # ffmpeg_decoder.cc
    # gstreamer_sink.cc
)

set(AV_HEADERS
    # ffmpeg_decoder.h
    # gstreamer_sink.h
)

# Find FFmpeg
# find_package(FFmpeg COMPONENTS avcodec avformat avutil)

# Find GStreamer
# find_package(PkgConfig REQUIRED)
# pkg_check_modules(GST REQUIRED gstreamer-1.0)

if(AV_SOURCES)
    add_library(atsc3_av ${AV_SOURCES})

    target_include_directories(atsc3_av
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<INSTALL_INTERFACE:include>
    )

    # Note: GStreamer pipeline must be built on main thread
    # FFmpeg decode workers can be threaded
endif()
