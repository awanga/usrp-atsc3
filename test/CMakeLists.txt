# test/ - Test suite
# Unit tests, integration tests, IQ capture replay tests

if(NOT ATSC3_BUILD_TESTS)
    message(STATUS "Tests disabled - set ATSC3_BUILD_TESTS=ON to enable")
    return()
endif()

# Unit tests (no hardware, no IQ files)
add_subdirectory(unit)

# Integration tests (may require IQ captures from git-lfs)
add_subdirectory(integration)

# Hardware tests (require physical USRP)
add_subdirectory(hw)

# Compliance tests (ATSC A/322 conformance)
add_subdirectory(compliance)

# Add label for easy test selection
# Unit tests: ctest -L unit
# Integration tests: ctest -L integration
# All tests: ctest
