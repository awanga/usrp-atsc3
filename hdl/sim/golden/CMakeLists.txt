# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Alfred Wanga
#
# hdl/sim/golden/ - Golden-vector CLIs, one per RTL block under test.
#
# Each target links the real atsc3_lib so cocotb's bit-exact comparison is
# always against the actual golden model. Only built when HDL stubs are
# enabled (see hdl/CMakeLists.txt) -- these are verification tools, not
# part of the shipping library.

add_executable(cordic_gen cordic_gen.cc)
target_link_libraries(cordic_gen atsc3_lib)

add_executable(bootstrap_gen bootstrap_gen.cc)
target_link_libraries(bootstrap_gen atsc3_lib)

add_executable(cp_removal_gen cp_removal_gen.cc)
target_link_libraries(cp_removal_gen atsc3_lib)

add_executable(timing_recovery_gen timing_recovery_gen.cc)
target_link_libraries(timing_recovery_gen atsc3_lib)

add_executable(fft_twiddle_gen fft_twiddle_gen.cc)
target_link_libraries(fft_twiddle_gen atsc3_lib)

add_executable(fft_engine_gen fft_engine_gen.cc)
target_link_libraries(fft_engine_gen atsc3_lib)

add_executable(pilot_cp_rom_gen pilot_cp_rom_gen.cc)
target_link_libraries(pilot_cp_rom_gen atsc3_lib)

add_executable(pilot_additional_cp_gen pilot_additional_cp_gen.cc)
target_link_libraries(pilot_additional_cp_gen atsc3_lib)

add_executable(pilot_extractor_gen pilot_extractor_gen.cc)
target_link_libraries(pilot_extractor_gen atsc3_lib)

add_executable(freq_correction_gen freq_correction_gen.cc)
target_link_libraries(freq_correction_gen atsc3_lib)

add_executable(channel_estimator_gen channel_estimator_gen.cc)
target_link_libraries(channel_estimator_gen atsc3_lib)

add_executable(equalizer_gen equalizer_gen.cc)
target_link_libraries(equalizer_gen atsc3_lib)

add_executable(demapper_gen demapper_gen.cc)
target_link_libraries(demapper_gen atsc3_lib)

add_executable(demapper_rom_gen demapper_rom_gen.cc)
target_link_libraries(demapper_rom_gen atsc3_lib)
