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

        // Load continual pilot positions for 8K
        continual_pilots_8k_.clear();
        auto cp_8k = root.get_child_optional("pilot_patterns.continual_pilots_8k");
        if (cp_8k) {
            for (const auto& p : *cp_8k) {
                continual_pilots_8k_.push_back(p.second.get_value<size_t>());
            }
        }

        // Load expected pilot counts
        expected_counts_.clear();
        auto exp_counts = root.get_child_optional("pilot_patterns.expected_pilot_counts");
        if (exp_counts) {
            for (const auto& fft : *exp_counts) {
                // Parse FFT size from key (e.g., "8K" -> 8192)
                size_t fft_size = 0;
                if (fft.first == "8K") {
                    fft_size = 8192;
                } else if (fft.first == "16K") {
                    fft_size = 16384;
                } else if (fft.first == "32K") {
                    fft_size = 32768;
                } else {
                    continue;
                }

                ExpectedPilotCounts counts;
                counts.continual = fft.second.get<size_t>("continual");

                auto scattered = fft.second.get_child_optional("scattered_per_symbol");
                if (scattered) {
                    for (const auto& s : *scattered) {
                        counts.scattered_per_symbol[s.first] =
                            s.second.get_value<size_t>();
                    }
                }

                expected_counts_[fft_size] = counts;
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
    if (index < 1 || index > 8) {
        return nullptr;
    }
    std::string name = "PP" + std::to_string(index);
    return get_pilot_pattern(name);
}

const FftSizeConfig* ModeConfig::get_fft_config(size_t fft_size) const {
    auto it = fft_configs_.find(fft_size);
    if (it != fft_configs_.end()) {
        return &it->second;
    }
    return nullptr;
}

const ExpectedPilotCounts* ModeConfig::get_expected_counts(size_t fft_size) const {
    auto it = expected_counts_.find(fft_size);
    if (it != expected_counts_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::string get_default_config_path() {
    // Try common locations
    std::vector<std::string> paths = {
        "config/atsc3_modes.json",
        "../config/atsc3_modes.json",
        "../../config/atsc3_modes.json",
        "/usr/share/atsc3/config/atsc3_modes.json",
        "/usr/local/share/atsc3/config/atsc3_modes.json"
    };

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
