# test/integration/ - Integration tests
# Tests that may require IQ captures from test/captures/ (git-lfs)

set(INTEGRATION_TEST_SOURCES
    # test_signal_chain.cc
    # test_iq_replay.cc
)

# Check if git-lfs captures are available
if(EXISTS "${CMAKE_SOURCE_DIR}/test/captures/.gitkeep")
    set(IQ_CAPTURES_AVAILABLE TRUE)
else()
    set(IQ_CAPTURES_AVAILABLE FALSE)
    message(STATUS "IQ captures not available - some integration tests will be skipped")
endif()

foreach(test_source ${INTEGRATION_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)

    add_executable(${test_name} ${test_source})

    target_link_libraries(${test_name}
        PRIVATE
            atsc3_lib
            GTest::gtest
            GTest::gtest_main
    )

    # Register with CTest
    gtest_discover_tests(${test_name}
        PROPERTIES
            LABELS "integration"
    )
endforeach()
