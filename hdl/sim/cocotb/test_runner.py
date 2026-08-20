"""pytest entry point: builds each RTL block with Verilator and runs its
cocotb testbench. This is the file pytest collects; the actual `@cocotb.test`
coroutines live in the test_<block>.py modules alongside it and only run
inside the simulator process the runner launches.

Run: hdl/sim/.venv/bin/python -m pytest hdl/sim/cocotb/test_runner.py -v
"""

import pathlib

from cocotb.runner import get_runner

HDL_ROOT = pathlib.Path(__file__).resolve().parents[2]
RTL_INCLUDE = HDL_ROOT / "rtl" / "include"
RTL_COMMON = HDL_ROOT / "rtl" / "common"
SIM_BUILD = HDL_ROOT / "sim" / "cocotb" / "sim_build"


def test_axi4s_skid_buffer():
    runner = get_runner("verilator")
    runner.build(
        verilog_sources=[RTL_COMMON / "axi4s_skid_buffer.v"],
        includes=[RTL_INCLUDE],
        hdl_toplevel="axi4s_skid_buffer",
        parameters={"DATA_WIDTH": 8},
        build_dir=SIM_BUILD / "axi4s_skid_buffer",
        always=True,
        # 1364-2001 is a language-mode constraint for lint (see the lint
        # gate); Verilator's simulation frontend doesn't take a matching
        # --language flag alongside cocotb's own generated wrapper, so
        # that check runs separately (see hdl/synth/lint.sh), not here.
        build_args=["-Wall"],
    )
    runner.test(
        hdl_toplevel="axi4s_skid_buffer",
        test_module="test_axi4s_skid_buffer",
        build_dir=SIM_BUILD / "axi4s_skid_buffer",
    )
