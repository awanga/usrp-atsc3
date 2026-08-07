// av_player_python.cc — pybind11 bindings for av_player block

#include "atsc3_av_player.h"
#include "docstrings.h"

#include <gnuradio/sync_block.h>
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_av_player(py::module_& m) {
    // Expose PlayerState enum
    py::enum_<gr::atsc3::PlayerState>(m, "PlayerState")
        .value("STOPPED", gr::atsc3::PlayerState::STOPPED)
        .value("PLAYING", gr::atsc3::PlayerState::PLAYING)
        .value("PAUSED", gr::atsc3::PlayerState::PAUSED)
        .value("BUFFERING", gr::atsc3::PlayerState::BUFFERING)
        .value("ERROR", gr::atsc3::PlayerState::ERROR)
        .export_values();

    py::class_<gr::atsc3::av_player, gr::sync_block, gr::block, gr::basic_block,
               std::shared_ptr<gr::atsc3::av_player>>(m, "av_player", DOC_AV_PLAYER)

        .def(py::init(&gr::atsc3::av_player::make), py::arg("enable_video") = true,
             py::arg("enable_audio") = true, py::arg("volume") = 1.0f, DOC_AV_PLAYER_MAKE)

        .def("play", &gr::atsc3::av_player::play, DOC_AV_PLAYER_PLAY)

        .def("pause", &gr::atsc3::av_player::pause, DOC_AV_PLAYER_PAUSE)

        .def("stop", &gr::atsc3::av_player::stop, DOC_AV_PLAYER_STOP)

        .def("set_volume", &gr::atsc3::av_player::set_volume, py::arg("volume"),
             DOC_AV_PLAYER_SET_VOLUME)

        .def("get_volume", &gr::atsc3::av_player::get_volume, DOC_AV_PLAYER_GET_VOLUME)

        .def("get_state", &gr::atsc3::av_player::get_state, DOC_AV_PLAYER_GET_STATE)

        .def("get_video_frames", &gr::atsc3::av_player::get_video_frames,
             DOC_AV_PLAYER_GET_VIDEO_FRAMES)

        .def("get_audio_samples", &gr::atsc3::av_player::get_audio_samples,
             DOC_AV_PLAYER_GET_AUDIO_SAMPLES);
}
