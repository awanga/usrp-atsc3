# hdl/sim/golden/ - Golden-vector CLIs, one per RTL block under test.
#
# Each target links the real atsc3_lib so cocotb's bit-exact comparison is
# always against the actual golden model. Only built when HDL stubs are
# enabled (see hdl/CMakeLists.txt) -- these are verification tools, not
# part of the shipping library.

add_executable(cordic_gen cordic_gen.cc)
target_link_libraries(cordic_gen atsc3_lib)
