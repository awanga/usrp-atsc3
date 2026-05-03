#pragma once

// ConfigBus — ATSC 3.0 runtime configuration distribution
//
// Simple pub/sub mechanism for distributing Atsc3Config from L1 decoder
// to all downstream processing blocks. Designed for RTL portability:
// - Fixed-size observer array (no dynamic allocation after init)
// - No heap allocation during publish
// - Thread-safe for single-writer, multiple-reader pattern
//
// Usage:
//   // During initialization
//   ConfigBus bus;
//   bus.register_observer(&channel_estimator);
//   bus.register_observer(&equalizer);
//   bus.register_observer(&demapper);
//
//   // When L1 decoder extracts new config
//   bus.publish(new_config);  // All observers notified
//
// Reference: ATSC A/322 Section 5 (L1 Signaling)

#include "atsc3_config.h"

#include <array>
#include <atomic>
#include <cstddef>

namespace atsc3 {
namespace config {

// Observer interface for configuration changes
// Implement this in any block that needs L1 configuration
class ConfigObserver {
public:
    virtual ~ConfigObserver() = default;

    // Called when new configuration is available
    // config: New Atsc3Config from L1 decoder
    // Note: Called synchronously from publish() - keep handler fast
    virtual void on_config_update(const Atsc3Config& config) = 0;
};

// Configuration distribution bus
// Single publisher (L1 decoder), multiple subscribers (DSP blocks)
class ConfigBus {
public:
    // Maximum number of observers (fixed for RTL portability)
    static constexpr size_t MAX_OBSERVERS = 16;

    ConfigBus();
    ~ConfigBus();

    // Register an observer to receive config updates
    // observer: Pointer to observer (must remain valid until unregistered)
    // Returns: true if registered, false if MAX_OBSERVERS reached
    // Note: Call only during initialization phase
    bool register_observer(ConfigObserver* observer);

    // Unregister an observer
    // observer: Previously registered observer
    // Note: Safe to call if observer not registered
    void unregister_observer(ConfigObserver* observer);

    // Publish new configuration to all observers
    // config: New configuration from L1 decoder
    // Note: Calls on_config_update() on each observer synchronously
    void publish(const Atsc3Config& config);

    // Get current configuration
    // Returns: Most recently published config
    const Atsc3Config& get_config() const {
        return current_config_;
    }

    // Check if configuration is valid
    bool has_valid_config() const {
        return current_config_.valid;
    }

    // Get number of registered observers
    size_t num_observers() const {
        return num_observers_;
    }

    // Check if an observer is registered
    bool is_registered(const ConfigObserver* observer) const;

    // Clear all observers (for testing/reset)
    void clear_observers();

    // Get publish count (for diagnostics)
    uint32_t get_publish_count() const {
        return publish_count_;
    }

private:
    // Current configuration
    Atsc3Config current_config_;

    // Fixed-size observer array (no dynamic allocation)
    std::array<ConfigObserver*, MAX_OBSERVERS> observers_;

    // Number of registered observers
    size_t num_observers_;

    // Publish counter for diagnostics
    uint32_t publish_count_;
};

// Global config bus instance (optional singleton pattern)
// Can be used when single bus is sufficient
ConfigBus& get_global_config_bus();

}  // namespace config
}  // namespace atsc3
