// constellation_demapper_python.cc — pybind11 bindings for constellation_demapper block

#include "atsc3_constellation_demapper.h"
#include "docstrings.h"

#include <gnuradio/sync_interpolator.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_constellation_demapper(py::module_& m) {
    py::class_<gr::atsc3::constellation_demapper, gr::sync_interpolator, gr::sync_block, gr::block,
               gr::basic_block, std::shared_ptr<gr::atsc3::constellation_demapper>>(
        m, "constellation_demapper", DOC_CONSTELLATION_DEMAPPER)

        .def(py::init(&gr::atsc3::constellation_demapper::make), py::arg("modulation") = 2,
             py::arg("code_rate") = 5, py::arg("noise_variance") = 0.1f,
             DOC_CONSTELLATION_DEMAPPER_MAKE)

        .def("set_modulation", &gr::atsc3::constellation_demapper::set_modulation,
             py::arg("modulation"), DOC_CONSTELLATION_DEMAPPER_SET_MODULATION)

        .def("set_code_rate", &gr::atsc3::constellation_demapper::set_code_rate,
             py::arg("code_rate"), DOC_CONSTELLATION_DEMAPPER_SET_CODE_RATE)

        .def("set_noise_variance", &gr::atsc3::constellation_demapper::set_noise_variance,
             py::arg("noise_variance"), DOC_CONSTELLATION_DEMAPPER_SET_NOISE_VARIANCE)

        .def("get_bits_per_symbol", &gr::atsc3::constellation_demapper::get_bits_per_symbol,
             DOC_CONSTELLATION_DEMAPPER_GET_BITS_PER_SYMBOL);
}
