"""pytest entry point: builds each RTL block with Verilator and with Icarus
and runs its cocotb testbench on both. This is the file pytest collects;
the actual `@cocotb.test` coroutines live in the test_<block>.py modules
alongside it and only run inside the simulator process the runner
launches.

Run: hdl/sim/.venv/bin/python -m pytest hdl/sim/cocotb/test_runner.py -v

HDL_RTL_DIR overrides the RTL tree; HDL_SIM_BUILD overrides the build
directory (default hdl/build/sim).
"""

import os
import pathlib
import xml.etree.ElementTree as ET

import pytest
from cocotb.runner import get_runner

HDL_ROOT = pathlib.Path(__file__).resolve().parents[2]
RTL = pathlib.Path(os.environ.get("HDL_RTL_DIR", HDL_ROOT / "rtl"))
SIM_BUILD = pathlib.Path(os.environ.get("HDL_SIM_BUILD", HDL_ROOT / "build" / "sim"))
SIMULATORS = ["verilator", "icarus"]

# toplevel -> (sources relative to RTL, extra include dirs relative to RTL,
#              parameters); the testbench is test_<toplevel>.py
BLOCKS = {
    "axi4s_skid_buffer": (["common/axi4s_skid_buffer.v"], [], {"DATA_WIDTH": 8}),
    "udiv_seq": (["common/udiv_seq.v"], [], {"WIDTH": 16, "CNT_WIDTH": 5}),
    "cordic": (["common/cordic.v"], [], {}),
    "bootstrap_detector": (
        ["sync/bootstrap_detector.v", "common/cordic.v", "common/udiv_seq.v"],
        [],
        {},
    ),
    "polyphase_fir": (["sync/polyphase_fir.v"], ["sync"], {}),
    "timing_recovery": (
        ["sync/timing_recovery.v", "sync/polyphase_fir.v"],
        ["sync"],
        {},
    ),
    "cp_removal": (["ofdm/cp_removal.v"], [], {}),
    "fft_engine": (
        ["ofdm/fft_engine.v"],
        [],
        {"TWIDDLE_HEX": f'"{RTL / "ofdm" / "fft_twiddles.hex"}"'},
    ),
}

# Verilator's generated C++ wrapper does not take --language, so the
# 1364-2001 restriction is enforced by hdl/synth/lint.sh, not here.
BUILD_ARGS = {"verilator": ["-Wall"], "icarus": ["-Wall"]}


def _check_results(results_xml):
    """cocotb's own check only fails on failed tests; also fail when the
    module ran no tests at all (e.g. a renamed or mis-imported module)."""
    cases = ET.parse(results_xml).getroot().iter("testcase")
    ran = [c for c in cases if c.find("skipped") is None]
    assert ran, f"{results_xml}: no cocotb tests ran"


def _run(sim, toplevel, sources, includes, parameters, test_module):
    build_dir = SIM_BUILD / sim / toplevel
    runner = get_runner(sim)
    runner.build(
        verilog_sources=sources,
        includes=[RTL / "include", *includes],
        hdl_toplevel=toplevel,
        parameters=parameters,
        build_dir=build_dir,
        always=True,
        build_args=BUILD_ARGS[sim],
        timescale=("1ns", "1ps"),
    )
    results = runner.test(
        hdl_toplevel=toplevel,
        test_module=test_module,
        build_dir=build_dir,
    )
    _check_results(results)


@pytest.mark.parametrize("sim", SIMULATORS)
@pytest.mark.parametrize("block", list(BLOCKS))
def test_block(block, sim):
    sources, includes, parameters = BLOCKS[block]
    _run(
        sim,
        block,
        [RTL / s for s in sources],
        [RTL / d for d in includes],
        parameters,
        f"test_{block}",
    )
