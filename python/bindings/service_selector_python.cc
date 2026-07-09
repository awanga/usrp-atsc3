// service_selector_python.cc — pybind11 bindings for service_selector block

#include "atsc3_service_selector.h"
#include "docstrings.h"

#include <gnuradio/block.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_service_selector(py::module_& m) {
    py::class_<gr::atsc3::service_selector, gr::block, gr::basic_block,
               std::shared_ptr<gr::atsc3::service_selector>>(m, "service_selector",
                                                             DOC_SERVICE_SELECTOR)

        .def(py::init(&gr::atsc3::service_selector::make), py::arg("service_id") = 0,
             DOC_SERVICE_SELECTOR_MAKE)

        .def("set_service_id", &gr::atsc3::service_selector::set_service_id, py::arg("service_id"),
             DOC_SERVICE_SELECTOR_SET_SERVICE_ID)

        .def("get_service_id", &gr::atsc3::service_selector::get_service_id,
             DOC_SERVICE_SELECTOR_GET_SERVICE_ID)

        .def("get_video_bytes", &gr::atsc3::service_selector::get_video_bytes,
             DOC_SERVICE_SELECTOR_GET_VIDEO_BYTES)

        .def("get_audio_bytes", &gr::atsc3::service_selector::get_audio_bytes,
             DOC_SERVICE_SELECTOR_GET_AUDIO_BYTES)

        .def("is_service_locked", &gr::atsc3::service_selector::is_service_locked,
             DOC_SERVICE_SELECTOR_IS_SERVICE_LOCKED);
}
