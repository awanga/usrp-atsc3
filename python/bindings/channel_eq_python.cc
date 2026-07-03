// channel_eq_python.cc — pybind11 bindings for channel_eq block

#include "atsc3_channel_eq.h"
#include "docstrings.h"

#include <gnuradio/sync_block.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_channel_eq(py::module_& m) {
    py::class_<gr::atsc3::channel_eq, gr::sync_block, gr::block, gr::basic_block,
               std::shared_ptr<gr::atsc3::channel_eq>>(m, "channel_eq", DOC_CHANNEL_EQ)

        .def(py::init(&gr::atsc3::channel_eq::make), py::arg("fft_size") = 8192,
             py::arg("pilot_pattern") = 3, py::arg("use_mmse") = true, DOC_CHANNEL_EQ_MAKE)

        .def("set_pilot_pattern", &gr::atsc3::channel_eq::set_pilot_pattern, py::arg("pattern"),
             DOC_CHANNEL_EQ_SET_PILOT_PATTERN)

        .def("set_mmse", &gr::atsc3::channel_eq::set_mmse, py::arg("use_mmse"),
             DOC_CHANNEL_EQ_SET_MMSE)

        .def("get_snr_db", &gr::atsc3::channel_eq::get_snr_db, DOC_CHANNEL_EQ_GET_SNR_DB)

        .def("get_mer_db", &gr::atsc3::channel_eq::get_mer_db, DOC_CHANNEL_EQ_GET_MER_DB);
}
