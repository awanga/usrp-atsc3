# test/hw/ - Hardware tests
# Tests that require physical USRP hardware
# Run with: ctest -L hw

# Only build if UHD is available
if(NOT UHD_FOUND)
    message(STATUS "UHD not found - hardware tests will not be built")
    return()
endif()

set(HW_TEST_SOURCES
    test_uhd_source.cc
)

foreach(test_source ${HW_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)

    add_executable(${test_name} ${test_source})

    target_link_libraries(${test_name}
        PRIVATE
            atsc3_hal
            ${UHD_LIBRARIES}
            GTest::gtest
            GTest::gtest_main
    )

    target_include_directories(${test_name}
        PRIVATE
            ${UHD_INCLUDE_DIRS}
    )

    target_compile_definitions(${test_name}
        PRIVATE
            ATSC3_HAS_UHD=1
    )

    # Register with CTest - labeled as "hw" for hardware tests
    gtest_discover_tests(${test_name}
        PROPERTIES
            LABELS "hw"
    )
endforeach()
