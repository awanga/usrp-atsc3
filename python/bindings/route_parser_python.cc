// route_parser_python.cc — pybind11 bindings for route_parser block

#include "atsc3_route_parser.h"
#include "docstrings.h"

#include <gnuradio/sync_block.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_route_parser(py::module_& m) {
    py::class_<gr::atsc3::route_parser, gr::sync_block, gr::block, gr::basic_block,
               std::shared_ptr<gr::atsc3::route_parser>>(m, "route_parser", DOC_ROUTE_PARSER)

        .def(py::init(&gr::atsc3::route_parser::make), DOC_ROUTE_PARSER_MAKE)

        .def("get_service_count", &gr::atsc3::route_parser::get_service_count,
             DOC_ROUTE_PARSER_GET_SERVICE_COUNT)

        .def("get_packet_count", &gr::atsc3::route_parser::get_packet_count,
             DOC_ROUTE_PARSER_GET_PACKET_COUNT)

        .def("get_error_count", &gr::atsc3::route_parser::get_error_count,
             DOC_ROUTE_PARSER_GET_ERROR_COUNT)

        .def("reset", &gr::atsc3::route_parser::reset, DOC_ROUTE_PARSER_RESET);
}
