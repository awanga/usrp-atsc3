# test/unit/ - Unit tests
# One .cc file per lib/ or hal/ class, using GoogleTest

#------------------------------------------------------------------------------
# lib/ tests (DSP library)
#------------------------------------------------------------------------------
set(LIB_TEST_SOURCES
    test_types.cc
    test_atsc3_config.cc
    test_bootstrap_detector.cc
    test_cp_removal.cc
    test_fft_engine.cc
    test_freq_correction.cc
    test_timing_recovery.cc
    test_frame_sync.cc
    test_pilot_extractor.cc
    test_channel_estimator.cc
    test_wiener_interpolator.cc
    test_equalizer.cc
    test_constellation_demapper.cc
    test_cell_deinterleaver.cc
    test_time_deinterleaver.cc
    test_freq_deinterleaver.cc
    test_ldpc_decoder.cc
    test_bch_decoder.cc
)

foreach(test_source ${LIB_TEST_SOURCES})
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

#------------------------------------------------------------------------------
# hal/ tests (Hardware Abstraction Layer)
#------------------------------------------------------------------------------
set(HAL_TEST_SOURCES
    test_null_source.cc
    test_file_source.cc
    test_agc.cc
)

foreach(test_source ${HAL_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)

    add_executable(${test_name} ${test_source})

    target_link_libraries(${test_name}
        PRIVATE
            atsc3_hal
            GTest::gtest
            GTest::gtest_main
    )

    # Register with CTest
    gtest_discover_tests(${test_name}
        PROPERTIES
            LABELS "unit"
    )
endforeach()
