# FindFFTW3.cmake
# ----------------
# Find the FFTW3 library (single and/or double precision)
#
# This module defines:
#   FFTW3_FOUND        - True if FFTW3 was found
#   FFTW3_INCLUDE_DIRS - FFTW3 include directories
#   FFTW3_LIBRARIES    - FFTW3 libraries to link against
#   FFTW3_VERSION      - FFTW3 version string (if available)
#
# Components:
#   FLOAT   - Single precision (libfftw3f)
#   DOUBLE  - Double precision (libfftw3)
#   THREADS - Thread-safe versions
#
# Usage:
#   find_package(FFTW3 REQUIRED)
#   find_package(FFTW3 REQUIRED COMPONENTS FLOAT THREADS)

include(FindPackageHandleStandardArgs)

# Find include directory
find_path(FFTW3_INCLUDE_DIR
    NAMES fftw3.h
    PATHS
        /usr/include
        /usr/local/include
        /opt/local/include
        $ENV{FFTW3_ROOT}/include
    DOC "FFTW3 include directory"
)

# Find double precision library
find_library(FFTW3_LIBRARY
    NAMES fftw3
    PATHS
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /usr/local/lib
        /opt/local/lib
        $ENV{FFTW3_ROOT}/lib
    DOC "FFTW3 double precision library"
)

# Find single precision library
find_library(FFTW3F_LIBRARY
    NAMES fftw3f
    PATHS
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /usr/local/lib
        /opt/local/lib
        $ENV{FFTW3_ROOT}/lib
    DOC "FFTW3 single precision library"
)

# Find thread-safe libraries
find_library(FFTW3_THREADS_LIBRARY
    NAMES fftw3_threads
    PATHS
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /usr/local/lib
        /opt/local/lib
        $ENV{FFTW3_ROOT}/lib
    DOC "FFTW3 double precision thread-safe library"
)

find_library(FFTW3F_THREADS_LIBRARY
    NAMES fftw3f_threads
    PATHS
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /usr/local/lib
        /opt/local/lib
        $ENV{FFTW3_ROOT}/lib
    DOC "FFTW3 single precision thread-safe library"
)

# Build library list based on components requested
set(FFTW3_LIBRARIES "")
set(FFTW3_INCLUDE_DIRS ${FFTW3_INCLUDE_DIR})

# Default: include both float and double precision
if(NOT FFTW3_FIND_COMPONENTS)
    set(FFTW3_FIND_COMPONENTS FLOAT DOUBLE)
endif()

foreach(component ${FFTW3_FIND_COMPONENTS})
    string(TOUPPER ${component} COMPONENT_UPPER)

    if(COMPONENT_UPPER STREQUAL "DOUBLE")
        if(FFTW3_LIBRARY)
            list(APPEND FFTW3_LIBRARIES ${FFTW3_LIBRARY})
            set(FFTW3_DOUBLE_FOUND TRUE)
        endif()
    elseif(COMPONENT_UPPER STREQUAL "FLOAT")
        if(FFTW3F_LIBRARY)
            list(APPEND FFTW3_LIBRARIES ${FFTW3F_LIBRARY})
            set(FFTW3_FLOAT_FOUND TRUE)
        endif()
    elseif(COMPONENT_UPPER STREQUAL "THREADS")
        if(FFTW3_THREADS_LIBRARY)
            list(APPEND FFTW3_LIBRARIES ${FFTW3_THREADS_LIBRARY})
        endif()
        if(FFTW3F_THREADS_LIBRARY)
            list(APPEND FFTW3_LIBRARIES ${FFTW3F_THREADS_LIBRARY})
        endif()
        if(FFTW3_THREADS_LIBRARY OR FFTW3F_THREADS_LIBRARY)
            set(FFTW3_THREADS_FOUND TRUE)
        endif()
    endif()
endforeach()

# Remove duplicates
list(REMOVE_DUPLICATES FFTW3_LIBRARIES)

# Try to determine version from pkg-config
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_FFTW3 QUIET fftw3)
    if(PC_FFTW3_VERSION)
        set(FFTW3_VERSION ${PC_FFTW3_VERSION})
    endif()
endif()

# Handle standard arguments
find_package_handle_standard_args(FFTW3
    REQUIRED_VARS FFTW3_INCLUDE_DIR FFTW3_LIBRARIES
    VERSION_VAR FFTW3_VERSION
    HANDLE_COMPONENTS
)

# Create imported targets
if(FFTW3_FOUND AND NOT TARGET FFTW3::fftw3)
    add_library(FFTW3::fftw3 UNKNOWN IMPORTED)
    set_target_properties(FFTW3::fftw3 PROPERTIES
        IMPORTED_LOCATION "${FFTW3_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFTW3_INCLUDE_DIR}"
    )
endif()

if(FFTW3_FLOAT_FOUND AND NOT TARGET FFTW3::fftw3f)
    add_library(FFTW3::fftw3f UNKNOWN IMPORTED)
    set_target_properties(FFTW3::fftw3f PROPERTIES
        IMPORTED_LOCATION "${FFTW3F_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFTW3_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(
    FFTW3_INCLUDE_DIR
    FFTW3_LIBRARY
    FFTW3F_LIBRARY
    FFTW3_THREADS_LIBRARY
    FFTW3F_THREADS_LIBRARY
)
