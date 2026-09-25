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
RTL_SYNC = HDL_ROOT / "rtl" / "sync"
RTL_OFDM = HDL_ROOT / "rtl" / "ofdm"
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


def test_cordic():
    runner = get_runner("verilator")
    runner.build(
        verilog_sources=[RTL_COMMON / "cordic.v"],
        includes=[RTL_INCLUDE],
        hdl_toplevel="cordic",
        build_dir=SIM_BUILD / "cordic",
        always=True,
        build_args=["-Wall"],
    )
    runner.test(
        hdl_toplevel="cordic",
        test_module="test_cordic",
        build_dir=SIM_BUILD / "cordic",
    )


def test_bootstrap_detector():
    runner = get_runner("verilator")
    runner.build(
        verilog_sources=[
            RTL_SYNC / "bootstrap_detector.v",
            RTL_COMMON / "cordic.v",
            RTL_COMMON / "udiv_seq.v",
        ],
        includes=[RTL_INCLUDE],
        hdl_toplevel="bootstrap_detector",
        build_dir=SIM_BUILD / "bootstrap_detector",
        always=True,
        build_args=["-Wall"],
    )
    runner.test(
        hdl_toplevel="bootstrap_detector",
        test_module="test_bootstrap_detector",
        build_dir=SIM_BUILD / "bootstrap_detector",
    )


def test_timing_recovery():
    runner = get_runner("verilator")
    runner.build(
        verilog_sources=[
            RTL_SYNC / "timing_recovery.v",
            RTL_SYNC / "polyphase_fir.v",
        ],
        includes=[RTL_INCLUDE, RTL_SYNC],
        hdl_toplevel="timing_recovery",
        build_dir=SIM_BUILD / "timing_recovery",
        always=True,
        build_args=["-Wall"],
    )
    runner.test(
        hdl_toplevel="timing_recovery",
        test_module="test_timing_recovery",
        build_dir=SIM_BUILD / "timing_recovery",
    )


def test_cp_removal():
    runner = get_runner("verilator")
    runner.build(
        verilog_sources=[RTL_OFDM / "cp_removal.v"],
        includes=[RTL_INCLUDE],
        hdl_toplevel="cp_removal",
        build_dir=SIM_BUILD / "cp_removal",
        always=True,
        build_args=["-Wall"],
    )
    runner.test(
        hdl_toplevel="cp_removal",
        test_module="test_cp_removal",
        build_dir=SIM_BUILD / "cp_removal",
    )
