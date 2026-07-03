// alp_demux_python.cc — pybind11 bindings for alp_demux block

#include "atsc3_alp_demux.h"
#include "docstrings.h"

#include <gnuradio/block.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_alp_demux(py::module_& m) {
    py::class_<gr::atsc3::alp_demux, gr::block, gr::basic_block,
               std::shared_ptr<gr::atsc3::alp_demux>>(m, "alp_demux", DOC_ALP_DEMUX)

        .def(py::init(&gr::atsc3::alp_demux::make), py::arg("plp_id") = 0, DOC_ALP_DEMUX_MAKE)

        .def("set_plp_id", &gr::atsc3::alp_demux::set_plp_id, py::arg("plp_id"),
             DOC_ALP_DEMUX_SET_PLP_ID)

        .def("get_plp_id", &gr::atsc3::alp_demux::get_plp_id, DOC_ALP_DEMUX_GET_PLP_ID)

        .def("get_packet_count", &gr::atsc3::alp_demux::get_packet_count,
             DOC_ALP_DEMUX_GET_PACKET_COUNT)

        .def("get_error_count", &gr::atsc3::alp_demux::get_error_count,
             DOC_ALP_DEMUX_GET_ERROR_COUNT);

    // Note: get_packet_stats() uses pointer output parameters and is not
    // bound here. Use get_packet_count() and get_error_count() instead.
}
