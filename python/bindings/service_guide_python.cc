// service_guide_python.cc — pybind11 bindings for service_guide block

#include "atsc3_service_guide.h"
#include "docstrings.h"

#include <gnuradio/block.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_service_guide(py::module_& m) {
    py::class_<gr::atsc3::service_guide, gr::block, gr::basic_block,
               std::shared_ptr<gr::atsc3::service_guide>>(m, "service_guide", DOC_SERVICE_GUIDE)

        .def(py::init(&gr::atsc3::service_guide::make), DOC_SERVICE_GUIDE_MAKE)

        .def("get_service_count", &gr::atsc3::service_guide::get_service_count,
             DOC_SERVICE_GUIDE_GET_SERVICE_COUNT)

        .def("get_service_json", &gr::atsc3::service_guide::get_service_json, py::arg("index"),
             DOC_SERVICE_GUIDE_GET_SERVICE_JSON)

        .def("get_all_services_json", &gr::atsc3::service_guide::get_all_services_json,
             DOC_SERVICE_GUIDE_GET_ALL_SERVICES_JSON);
}
