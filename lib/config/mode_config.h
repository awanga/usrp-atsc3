// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// Mode Configuration — ATSC 3.0 mode table loading from JSON
//
// Loads pilot patterns, FFT sizes, CP fractions, and other mode parameters
// from config/atsc3_modes.json at runtime.
//
// Reference: ATSC A/322 Section 7.2 (Pilot Patterns)

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace atsc3 {
namespace config {

// Pilot pattern definition
struct PilotPatternDef {
    int code;   // L1D_scattered_pilot_pattern code (PilotPattern enumerator value)
    size_t dx;  // Frequency spacing (subcarriers)
    size_t dy;  // Time spacing (symbols)
};

// FFT size configuration
struct FftSizeConfig {
    size_t nfft;
    size_t active_carriers;
};

// Mode configuration loaded from JSON
class ModeConfig {
public:
    // Get singleton instance
    static ModeConfig& instance();

    // Load configuration from JSON file
    // Returns true on success, false on error
    bool load(const std::string& json_path);

    // Check if configuration is loaded
    bool is_loaded() const {
        return loaded_;
    }

    // Get pilot pattern definition by name (SP3_2 to SP32_4)
    // Returns nullptr if pattern not found
    const PilotPatternDef* get_pilot_pattern(const std::string& name) const;

    // Get pilot pattern definition by L1D_scattered_pilot_pattern code (0-15)
    const PilotPatternDef* get_pilot_pattern(int index) const;

    // Get FFT size configuration
    const FftSizeConfig* get_fft_config(size_t fft_size) const;

    // Get all pilot patterns
    const std::map<std::string, PilotPatternDef>& get_all_patterns() const {
        return pilot_patterns_;
    }

private:
    ModeConfig() = default;
    ~ModeConfig() = default;
    ModeConfig(const ModeConfig&) = delete;
    ModeConfig& operator=(const ModeConfig&) = delete;

    bool loaded_ = false;

    // Pilot patterns (SP3_2 to SP32_4)
    std::map<std::string, PilotPatternDef> pilot_patterns_;

    // FFT configurations
    std::map<size_t, FftSizeConfig> fft_configs_;
};

// Convenience function to get default config path
std::string get_default_config_path();

}  // namespace config
}  // namespace atsc3
