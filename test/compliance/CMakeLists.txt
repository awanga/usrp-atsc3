# test/compliance/ - ATSC 3.0 Compliance Tests
# Conformance tests against ATSC A/322 specification
#
# Run with: ctest -L compliance

set(COMPLIANCE_TEST_SOURCES
    test_bootstrap_variants.cc
    test_l1_compliance.cc
    test_interleaver_compliance.cc
    test_nuc_compliance.cc
    test_ldpc_waterfall.cc
)

foreach(test_source ${COMPLIANCE_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)

    add_executable(${test_name} ${test_source})

    target_link_libraries(${test_name}
        PRIVATE
            atsc3_lib
            GTest::gtest
            GTest::gtest_main
    )

    # Register with CTest using "compliance" label
    gtest_discover_tests(${test_name}
        PROPERTIES
            LABELS "compliance"
    )
endforeach()
