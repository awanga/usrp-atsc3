# test/fuzz/ - libFuzzer harnesses for parser fuzzing
#
# Usage:
#   cmake .. -DATSC3_BUILD_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
#   cmake --build . --target fuzz_alp_demux
#   ./test/fuzz/fuzz_alp_demux corpus/alp/
#
# Requires: clang with libFuzzer support

if(NOT ATSC3_BUILD_FUZZ)
    message(STATUS "Fuzzing disabled - set ATSC3_BUILD_FUZZ=ON to enable")
    return()
endif()

# Check for clang (libFuzzer requires clang)
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "Fuzzing requires Clang compiler. Current: ${CMAKE_CXX_COMPILER_ID}")
    return()
endif()

# Common fuzzer compile flags
set(FUZZER_FLAGS "-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer")
set(FUZZER_LINK_FLAGS "-fsanitize=fuzzer,address,undefined")

# ALP Demux Fuzzer
add_executable(fuzz_alp_demux fuzz_alp_demux.cc)
target_link_libraries(fuzz_alp_demux PRIVATE atsc3_lib)
target_compile_options(fuzz_alp_demux PRIVATE ${FUZZER_FLAGS})
set_target_properties(fuzz_alp_demux PROPERTIES
    LINK_FLAGS "${FUZZER_LINK_FLAGS}"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# ROUTE Parser Fuzzer
add_executable(fuzz_route_parser fuzz_route_parser.cc)
target_link_libraries(fuzz_route_parser PRIVATE atsc3_lib)
target_compile_options(fuzz_route_parser PRIVATE ${FUZZER_FLAGS})
set_target_properties(fuzz_route_parser PROPERTIES
    LINK_FLAGS "${FUZZER_LINK_FLAGS}"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Create corpus directories
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/corpus/alp)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/corpus/route)

message(STATUS "Fuzzer targets: fuzz_alp_demux, fuzz_route_parser")
