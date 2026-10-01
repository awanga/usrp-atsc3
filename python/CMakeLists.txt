# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Alfred Wanga
#
# python/ - Python bindings for OpenATSC3

if(NOT Gnuradio_FOUND)
    message(STATUS "Skipping python/ - GNU Radio not found")
    return()
endif()

if(NOT pybind11_FOUND)
    message(STATUS "Skipping python/ - pybind11 not found")
    return()
endif()

# Add bindings subdirectory
add_subdirectory(bindings)

# Install Python package __init__.py
install(FILES __init__.py
    DESTINATION ${PYTHON_INSTALL_DIR}/gnuradio/atsc3
)
