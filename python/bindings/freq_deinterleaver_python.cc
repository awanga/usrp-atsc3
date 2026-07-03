// freq_deinterleaver_python.cc — pybind11 bindings for freq_deinterleaver block

#include "atsc3_freq_deinterleaver.h"
#include "docstrings.h"

#include <gnuradio/sync_block.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_freq_deinterleaver(py::module_& m) {
    py::class_<gr::atsc3::freq_deinterleaver, gr::sync_block, gr::block, gr::basic_block,
               std::shared_ptr<gr::atsc3::freq_deinterleaver>>(m, "freq_deinterleaver",
                                                               DOC_FREQ_DEINTERLEAVER)

        .def(py::init(&gr::atsc3::freq_deinterleaver::make), py::arg("fft_size") = 8192,
             DOC_FREQ_DEINTERLEAVER_MAKE)

        .def("set_fft_size", &gr::atsc3::freq_deinterleaver::set_fft_size, py::arg("fft_size"),
             DOC_FREQ_DEINTERLEAVER_SET_FFT_SIZE)

        .def("get_fft_size", &gr::atsc3::freq_deinterleaver::get_fft_size,
             DOC_FREQ_DEINTERLEAVER_GET_FFT_SIZE)

        .def("get_num_carriers", &gr::atsc3::freq_deinterleaver::get_num_carriers,
             DOC_FREQ_DEINTERLEAVER_GET_NUM_CARRIERS);
}
