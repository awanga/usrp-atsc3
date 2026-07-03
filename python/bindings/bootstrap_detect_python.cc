// bootstrap_detect_python.cc — pybind11 bindings for bootstrap_detect block

#include "atsc3_bootstrap_detect.h"
#include "docstrings.h"

#include <gnuradio/block.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_bootstrap_detect(py::module_& m) {
    py::class_<gr::atsc3::bootstrap_detect, gr::block, gr::basic_block,
               std::shared_ptr<gr::atsc3::bootstrap_detect>>(m, "bootstrap_detect",
                                                             DOC_BOOTSTRAP_DETECT)

        .def(py::init(&gr::atsc3::bootstrap_detect::make), py::arg("threshold") = 0.7f,
             DOC_BOOTSTRAP_DETECT_MAKE)

        .def("get_cfo_hz", &gr::atsc3::bootstrap_detect::get_cfo_hz,
             DOC_BOOTSTRAP_DETECT_GET_CFO_HZ)

        .def("is_locked", &gr::atsc3::bootstrap_detect::is_locked, DOC_BOOTSTRAP_DETECT_IS_LOCKED)

        .def("set_threshold", &gr::atsc3::bootstrap_detect::set_threshold, py::arg("threshold"),
             DOC_BOOTSTRAP_DETECT_SET_THRESHOLD);
}
