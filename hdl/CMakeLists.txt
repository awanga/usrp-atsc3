# hdl/ - RTL port stubs and AXI4-S interface definitions (post-MVP)
# No vendor DSP primitives (DSP48, Xilinx/Altera IP). Generic RTL only.

if(NOT ATSC3_ENABLE_HDL_STUBS)
    message(STATUS "HDL stubs disabled - set ATSC3_ENABLE_HDL_STUBS=ON to enable")
    return()
endif()

# Verilator for simulation (optional)
find_program(VERILATOR verilator)
if(VERILATOR)
    message(STATUS "Found Verilator: ${VERILATOR}")

    # Verilog-2001 lint gate (see hdl/synth/lint.sh, hdl/docs/toolchain.md)
    add_custom_target(hdl_lint
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/synth/lint.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Linting hdl/rtl/ (Verilog-2001, zero warnings)"
    )
else()
    message(WARNING "Verilator not found - HDL simulation will be unavailable")
endif()

# SymbiYosys for formal verification (optional)
find_program(SBY sby)
if(SBY)
    message(STATUS "Found SymbiYosys: ${SBY}")
else()
    message(WARNING "SymbiYosys (sby) not found - HDL formal verification will be unavailable")
endif()

# cocotb runs from its own project-local venv (see hdl/docs/toolchain.md);
# not wired as a CMake target since pytest's own discovery already covers
# hdl/sim/cocotb/test_runner.py, and neither cocotb nor SymbiYosys benefit
# from CMake's C/C++-oriented build-graph model. Invoke directly:
#   hdl/sim/cocotb/.venv/bin/python -m pytest hdl/sim/cocotb/test_runner.py
