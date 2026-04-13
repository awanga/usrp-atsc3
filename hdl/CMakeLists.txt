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
else()
    message(WARNING "Verilator not found - HDL simulation will be unavailable")
endif()

# Custom target for HDL stub generation
# add_custom_target(hdl_stubs
#     COMMAND ${CMAKE_COMMAND} -E echo "Generating HDL stubs..."
#     WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/stubs
# )
