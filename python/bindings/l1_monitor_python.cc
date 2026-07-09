// l1_monitor_python.cc — pybind11 bindings for l1_monitor block

#include "atsc3_l1_monitor.h"
#include "docstrings.h"

#include <gnuradio/block.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_l1_monitor(py::module_& m) {
    py::class_<gr::atsc3::l1_monitor, gr::block, gr::basic_block,
               std::shared_ptr<gr::atsc3::l1_monitor>>(m, "l1_monitor", DOC_L1_MONITOR)

        .def(py::init(&gr::atsc3::l1_monitor::make), DOC_L1_MONITOR_MAKE)

        .def("get_fft_size", &gr::atsc3::l1_monitor::get_fft_size, DOC_L1_MONITOR_GET_FFT_SIZE)

        .def("get_pilot_pattern", &gr::atsc3::l1_monitor::get_pilot_pattern,
             DOC_L1_MONITOR_GET_PILOT_PATTERN)

        .def("get_num_plps", &gr::atsc3::l1_monitor::get_num_plps, DOC_L1_MONITOR_GET_NUM_PLPS)

        .def("is_config_valid", &gr::atsc3::l1_monitor::is_config_valid,
             DOC_L1_MONITOR_IS_CONFIG_VALID)

        .def("get_l1_json", &gr::atsc3::l1_monitor::get_l1_json, DOC_L1_MONITOR_GET_L1_JSON);
}
