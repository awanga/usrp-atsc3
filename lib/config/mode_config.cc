// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// mode_config.cc — ATSC 3.0 mode table loading from JSON

#include "mode_config.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <fstream>
#include <stdexcept>

namespace atsc3 {
namespace config {

namespace pt = boost::property_tree;

ModeConfig& ModeConfig::instance() {
    static ModeConfig instance;
    return instance;
}

bool ModeConfig::load(const std::string& json_path) {
    try {
        pt::ptree root;
        pt::read_json(json_path, root);

        // Load pilot patterns
        pilot_patterns_.clear();
        auto patterns = root.get_child_optional("pilot_patterns.patterns");
        if (patterns) {
            for (const auto& p : *patterns) {
                PilotPatternDef def;
                def.code = p.second.get<int>("code");
                def.dx = p.second.get<size_t>("dx");
                def.dy = p.second.get<size_t>("dy");
                pilot_patterns_[p.first] = def;
            }
        }

        // Load FFT configurations
        fft_configs_.clear();
        auto fft_sizes = root.get_child_optional("fft_sizes");
        if (fft_sizes) {
            for (const auto& f : *fft_sizes) {
                FftSizeConfig cfg;
                cfg.nfft = f.second.get<size_t>("nfft");
                cfg.active_carriers = f.second.get<size_t>("active_carriers");
                fft_configs_[cfg.nfft] = cfg;
            }
        }

        loaded_ = true;
        return true;
    } catch (const std::exception& e) {
        loaded_ = false;
        return false;
    }
}

const PilotPatternDef* ModeConfig::get_pilot_pattern(const std::string& name) const {
    auto it = pilot_patterns_.find(name);
    if (it != pilot_patterns_.end()) {
        return &it->second;
    }
    return nullptr;
}

const PilotPatternDef* ModeConfig::get_pilot_pattern(int index) const {
    // Pilot patterns are keyed by name; the code is the index into the table.
    for (const auto& entry : pilot_patterns_) {
        if (entry.second.code == index) {
            return &entry.second;
        }
    }
    return nullptr;
}

const FftSizeConfig* ModeConfig::get_fft_config(size_t fft_size) const {
    auto it = fft_configs_.find(fft_size);
    if (it != fft_configs_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::string get_default_config_path() {
    // Try common locations
    std::vector<std::string> paths = {"config/atsc3_modes.json", "../config/atsc3_modes.json",
                                      "../../config/atsc3_modes.json",
                                      "/usr/share/atsc3/config/atsc3_modes.json",
                                      "/usr/local/share/atsc3/config/atsc3_modes.json"};

    for (const auto& path : paths) {
        std::ifstream f(path);
        if (f.good()) {
            return path;
        }
    }

    return "config/atsc3_modes.json";
}

}  // namespace config
}  // namespace atsc3
