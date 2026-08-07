// time_deinterleaver_python.cc — pybind11 bindings for time_deinterleaver block

#include "atsc3_time_deinterleaver.h"
#include "docstrings.h"

#include <gnuradio/sync_block.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_time_deinterleaver(py::module_& m) {
    py::class_<gr::atsc3::time_deinterleaver, gr::sync_block, gr::block, gr::basic_block,
               std::shared_ptr<gr::atsc3::time_deinterleaver>>(m, "time_deinterleaver",
                                                               DOC_TIME_DEINTERLEAVER)

        .def(py::init(&gr::atsc3::time_deinterleaver::make), py::arg("ti_mode") = 1,
             py::arg("ti_depth") = 0, DOC_TIME_DEINTERLEAVER_MAKE)

        .def("set_ti_mode", &gr::atsc3::time_deinterleaver::set_ti_mode, py::arg("ti_mode"),
             DOC_TIME_DEINTERLEAVER_SET_TI_MODE)

        .def("set_ti_depth", &gr::atsc3::time_deinterleaver::set_ti_depth, py::arg("ti_depth"),
             DOC_TIME_DEINTERLEAVER_SET_TI_DEPTH)

        .def("is_settled", &gr::atsc3::time_deinterleaver::is_settled,
             DOC_TIME_DEINTERLEAVER_IS_SETTLED)

        .def("get_settling_blocks", &gr::atsc3::time_deinterleaver::get_settling_blocks,
             DOC_TIME_DEINTERLEAVER_GET_SETTLING_BLOCKS)

        .def("reset", &gr::atsc3::time_deinterleaver::reset, DOC_TIME_DEINTERLEAVER_RESET);
}
