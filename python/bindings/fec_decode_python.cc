// fec_decode_python.cc — pybind11 bindings for fec_decode block

#include "atsc3_fec_decode.h"
#include "docstrings.h"

#include <gnuradio/block.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_fec_decode(py::module_& m) {
    py::class_<gr::atsc3::fec_decode, gr::block, gr::basic_block,
               std::shared_ptr<gr::atsc3::fec_decode>>(m, "fec_decode", DOC_FEC_DECODE)

        .def(py::init(&gr::atsc3::fec_decode::make), py::arg("code_rate") = 6,
             py::arg("codeword_length") = 64800, py::arg("max_iterations") = 50,
             DOC_FEC_DECODE_MAKE)

        .def("set_code_rate", &gr::atsc3::fec_decode::set_code_rate, py::arg("code_rate"),
             DOC_FEC_DECODE_SET_CODE_RATE)

        .def("set_codeword_length", &gr::atsc3::fec_decode::set_codeword_length, py::arg("length"),
             DOC_FEC_DECODE_SET_CODEWORD_LENGTH)

        .def("get_avg_iterations", &gr::atsc3::fec_decode::get_avg_iterations,
             DOC_FEC_DECODE_GET_AVG_ITERATIONS)

        .def("get_fer", &gr::atsc3::fec_decode::get_fer, DOC_FEC_DECODE_GET_FER)

        .def("last_converged", &gr::atsc3::fec_decode::last_converged,
             DOC_FEC_DECODE_LAST_CONVERGED);
}
