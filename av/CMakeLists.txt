# av/ - Audio/Video processing FFmpeg HEVC/AC-4 decode + GStreamer playback

# Find FFmpeg using pkg-config
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(AVCODEC QUIET libavcodec)
  pkg_check_modules(AVFORMAT QUIET libavformat)
  pkg_check_modules(AVUTIL QUIET libavutil)
  pkg_check_modules(SWRESAMPLE QUIET libswresample)
  pkg_check_modules(GST QUIET gstreamer-1.0)
  pkg_check_modules(GST_APP QUIET gstreamer-app-1.0)
endif()

# Check if FFmpeg is available
if(AVCODEC_FOUND
   AND AVUTIL_FOUND
   AND SWRESAMPLE_FOUND)
  set(FFMPEG_FOUND TRUE)
  message(
    STATUS "Found FFmpeg: avcodec=${AVCODEC_VERSION}, avutil=${AVUTIL_VERSION}")

  # Check for AC-4 codec ID (added in FFmpeg 4.3, but may be missing in some
  # builds)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_INCLUDES ${AVCODEC_INCLUDE_DIRS})
  set(CMAKE_REQUIRED_LIBRARIES ${AVCODEC_LIBRARIES})
  check_cxx_source_compiles(
    "
        extern \"C\" {
        #include <libavcodec/avcodec.h>
        }
        int main() { AVCodecID id = AV_CODEC_ID_AC4; (void)id; return 0; }
    "
    HAVE_AV_CODEC_ID_AC4)

  check_cxx_source_compiles(
    "
        extern \"C\" {
        #include <libavcodec/avcodec.h>
        }
        int main() { AVCodecID id = AV_CODEC_ID_MPEGH_3D_AUDIO; (void)id; return 0; }
    "
    HAVE_AV_CODEC_ID_MPEGH_3D_AUDIO)

  if(HAVE_AV_CODEC_ID_AC4)
    message(STATUS "FFmpeg has AC-4 codec support")
  else()
    message(STATUS "FFmpeg missing AC-4 codec ID - will use AAC fallback")
  endif()

  if(HAVE_AV_CODEC_ID_MPEGH_3D_AUDIO)
    message(STATUS "FFmpeg has MPEG-H 3D Audio codec support")
  else()
    message(
      STATUS "FFmpeg missing MPEG-H 3D Audio codec ID - will use AAC fallback")
  endif()
else()
  set(FFMPEG_FOUND FALSE)
  message(STATUS "FFmpeg not found - av/ decoders will not be built")
endif()

# Check if GStreamer is available
if(GST_FOUND AND GST_APP_FOUND)
  set(GSTREAMER_FOUND TRUE)
  message(STATUS "Found GStreamer: ${GST_VERSION}")
else()
  set(GSTREAMER_FOUND FALSE)
  message(STATUS "GStreamer not found - av/ playback will not be built")
endif()

set(AV_SOURCES)
set(AV_HEADERS)

# Add FFmpeg-based decoders if available
if(FFMPEG_FOUND)
  list(APPEND AV_SOURCES hevc_decoder.cc audio_decoder.cc)
  list(APPEND AV_HEADERS ring_buffer.h hevc_decoder.h audio_decoder.h)
endif()

# Add GStreamer playback if available
if(GSTREAMER_FOUND)
  list(APPEND AV_SOURCES gst_player.cc)
  list(APPEND AV_HEADERS gst_player.h)
endif()

if(AV_SOURCES)
  add_library(atsc3_av ${AV_SOURCES})

  # Enable PIC since this static lib is linked into shared library
  set_target_properties(atsc3_av PROPERTIES POSITION_INDEPENDENT_CODE ON)

  target_include_directories(
    atsc3_av PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                    $<INSTALL_INTERFACE:include>)

  # Link FFmpeg libraries
  if(FFMPEG_FOUND)
    target_include_directories(
      atsc3_av PRIVATE ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS}
                       ${SWRESAMPLE_INCLUDE_DIRS})
    target_link_libraries(
      atsc3_av PRIVATE ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES}
                       ${SWRESAMPLE_LIBRARIES})
    target_compile_options(atsc3_av PRIVATE ${AVCODEC_CFLAGS_OTHER})
    # Pass codec availability flags to source
    if(HAVE_AV_CODEC_ID_AC4)
      target_compile_definitions(atsc3_av PRIVATE ATSC3_HAS_AC4_CODEC=1)
    endif()
    if(HAVE_AV_CODEC_ID_MPEGH_3D_AUDIO)
      target_compile_definitions(atsc3_av PRIVATE ATSC3_HAS_MPEGH_CODEC=1)
    endif()
  endif()

  # Link GStreamer libraries
  if(GSTREAMER_FOUND)
    target_include_directories(atsc3_av PRIVATE ${GST_INCLUDE_DIRS}
                                                ${GST_APP_INCLUDE_DIRS})
    target_link_libraries(atsc3_av PRIVATE ${GST_LIBRARIES}
                                           ${GST_APP_LIBRARIES})
  endif()

  # Note: GStreamer pipeline must be built on main thread FFmpeg decode workers
  # can be threaded
endif()
