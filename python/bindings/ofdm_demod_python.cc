// ofdm_demod_python.cc — pybind11 bindings for ofdm_demod block

#include "atsc3_ofdm_demod.h"
#include "docstrings.h"

#include <gnuradio/sync_block.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_ofdm_demod(py::module_& m) {
    py::class_<gr::atsc3::ofdm_demod, gr::sync_block, gr::block, gr::basic_block,
               std::shared_ptr<gr::atsc3::ofdm_demod>>(m, "ofdm_demod", DOC_OFDM_DEMOD)

        .def(py::init(&gr::atsc3::ofdm_demod::make), py::arg("fft_size") = 8192,
             py::arg("cp_length") = 1024, DOC_OFDM_DEMOD_MAKE)

        .def("set_fft_size", &gr::atsc3::ofdm_demod::set_fft_size, py::arg("fft_size"),
             DOC_OFDM_DEMOD_SET_FFT_SIZE)

        .def("set_cp_length", &gr::atsc3::ofdm_demod::set_cp_length, py::arg("cp_length"),
             DOC_OFDM_DEMOD_SET_CP_LENGTH)

        .def("get_fft_size", &gr::atsc3::ofdm_demod::get_fft_size, DOC_OFDM_DEMOD_GET_FFT_SIZE)

        .def("get_cp_length", &gr::atsc3::ofdm_demod::get_cp_length, DOC_OFDM_DEMOD_GET_CP_LENGTH);
}
