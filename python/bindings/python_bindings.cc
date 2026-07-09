// python_bindings.cc — Main pybind11 module for gr-atsc3

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

// Forward declarations for per-block binding functions
void bind_bootstrap_detect(py::module_& m);
void bind_ofdm_demod(py::module_& m);
void bind_channel_eq(py::module_& m);
void bind_constellation_demapper(py::module_& m);
void bind_cell_deinterleaver(py::module_& m);
void bind_freq_deinterleaver(py::module_& m);
void bind_time_deinterleaver(py::module_& m);
void bind_fec_decode(py::module_& m);
void bind_alp_demux(py::module_& m);
void bind_route_parser(py::module_& m);
void bind_service_selector(py::module_& m);
void bind_av_player(py::module_& m);
void bind_l1_monitor(py::module_& m);
void bind_service_guide(py::module_& m);

PYBIND11_MODULE(atsc3_python, m) {
    m.doc() = R"doc(
gr-atsc3: GNU Radio OOT module for ATSC 3.0 physical-layer receiver

This module provides blocks for receiving ATSC 3.0 broadcast signals:
- bootstrap_detect: Bootstrap signal detection and coarse CFO estimation
- ofdm_demod: OFDM demodulation with FFT and CP removal
- channel_eq: Channel estimation and equalization
- constellation_demapper: QAM symbol to soft LLR conversion
- cell_deinterleaver: Cell de-interleaving
- freq_deinterleaver: Frequency de-interleaving
- time_deinterleaver: Time de-interleaving
- fec_decode: LDPC/BCH forward error correction decoding
- alp_demux: ALP packet demultiplexing
- route_parser: ROUTE/DASH signaling parser for service discovery
- service_selector: Service selection and ES extraction
- av_player: Live A/V playback via GStreamer
- l1_monitor: L1 signaling display
- service_guide: Service catalog display
)doc";

    // Import GNU Radio runtime to ensure proper type registration
    py::module_::import("gnuradio.gr");

    // Bind all blocks
    bind_bootstrap_detect(m);
    bind_ofdm_demod(m);
    bind_channel_eq(m);
    bind_constellation_demapper(m);
    bind_cell_deinterleaver(m);
    bind_freq_deinterleaver(m);
    bind_time_deinterleaver(m);
    bind_fec_decode(m);
    bind_alp_demux(m);
    bind_route_parser(m);
    bind_service_selector(m);
    bind_av_player(m);
    bind_l1_monitor(m);
    bind_service_guide(m);
}
