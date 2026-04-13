# test/unit/ - Unit tests
# One .cc file per lib/ class, using GoogleTest

set(UNIT_TEST_SOURCES
    test_types.cc
)

foreach(test_source ${UNIT_TEST_SOURCES})
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
            LABELS "unit"
    )
endforeach()
