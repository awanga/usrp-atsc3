// config_bus.cc — ATSC 3.0 configuration distribution
//
// Implements pub/sub for runtime configuration updates.
//
// Reference: ATSC A/322 Section 5 (L1 Signaling)

#include "config_bus.h"

#include <algorithm>

namespace atsc3 {
namespace config {

ConfigBus::ConfigBus() : num_observers_(0), publish_count_(0) {
    // Initialize observer array to nullptr
    observers_.fill(nullptr);

    // Initialize with default config
    current_config_ = Atsc3Config();
}

ConfigBus::~ConfigBus() {
    // Observers are not owned, just clear the array
    clear_observers();
}

bool ConfigBus::register_observer(ConfigObserver* observer) {
    if (observer == nullptr) {
        return false;
    }

    // Check if already registered
    if (is_registered(observer)) {
        return true;  // Already registered, consider success
    }

    // Check capacity
    if (num_observers_ >= MAX_OBSERVERS) {
        return false;  // No room
    }

    // Add to array
    observers_[num_observers_] = observer;
    num_observers_++;

    return true;
}

void ConfigBus::unregister_observer(ConfigObserver* observer) {
    if (observer == nullptr) {
        return;
    }

    // Find and remove observer
    for (size_t i = 0; i < num_observers_; i++) {
        if (observers_[i] == observer) {
            // Shift remaining observers down
            for (size_t j = i; j < num_observers_ - 1; j++) {
                observers_[j] = observers_[j + 1];
            }
            observers_[num_observers_ - 1] = nullptr;
            num_observers_--;
            return;
        }
    }
}

void ConfigBus::publish(const Atsc3Config& config) {
    // Update current config
    current_config_ = config;
    publish_count_++;

    // Notify all observers
    for (size_t i = 0; i < num_observers_; i++) {
        if (observers_[i] != nullptr) {
            observers_[i]->on_config_update(config);
        }
    }
}

bool ConfigBus::is_registered(const ConfigObserver* observer) const {
    if (observer == nullptr) {
        return false;
    }

    for (size_t i = 0; i < num_observers_; i++) {
        if (observers_[i] == observer) {
            return true;
        }
    }

    return false;
}

void ConfigBus::clear_observers() {
    observers_.fill(nullptr);
    num_observers_ = 0;
}

// Global instance
ConfigBus& get_global_config_bus() {
    static ConfigBus instance;
    return instance;
}

}  // namespace config
}  // namespace atsc3
