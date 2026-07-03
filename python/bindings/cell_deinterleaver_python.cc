// cell_deinterleaver_python.cc — pybind11 bindings for cell_deinterleaver block

#include "atsc3_cell_deinterleaver.h"
#include "docstrings.h"

#include <gnuradio/sync_block.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_cell_deinterleaver(py::module_& m) {
    py::class_<gr::atsc3::cell_deinterleaver, gr::sync_block, gr::block, gr::basic_block,
               std::shared_ptr<gr::atsc3::cell_deinterleaver>>(m, "cell_deinterleaver",
                                                               DOC_CELL_DEINTERLEAVER)

        .def(py::init(&gr::atsc3::cell_deinterleaver::make), py::arg("fft_size") = 8192,
             DOC_CELL_DEINTERLEAVER_MAKE)

        .def("set_fft_size", &gr::atsc3::cell_deinterleaver::set_fft_size, py::arg("fft_size"),
             DOC_CELL_DEINTERLEAVER_SET_FFT_SIZE)

        .def("get_fft_size", &gr::atsc3::cell_deinterleaver::get_fft_size,
             DOC_CELL_DEINTERLEAVER_GET_FFT_SIZE);
}
