// service_catalog.cc — Service Catalog implementation
//
// Maintains broadcast service list from ATSC 3.0 signaling

#include "service_catalog.h"

#include <algorithm>
#include <cstring>

namespace atsc3 {
namespace framing {

ServiceCatalog::ServiceCatalog() {
    clear();
}

ServiceCatalog::~ServiceCatalog() = default;

const Service* ServiceCatalog::get_service(size_t index) const {
    if (index >= num_services_) {
        return nullptr;
    }
    return &services_[index];
}

const Service* ServiceCatalog::find_service(uint16_t service_id) const {
    for (size_t i = 0; i < num_services_; i++) {
        if (services_[i].valid && services_[i].service_id == service_id) {
            return &services_[i];
        }
    }
    return nullptr;
}

const Service* ServiceCatalog::find_by_channel(uint16_t major, uint16_t minor) const {
    for (size_t i = 0; i < num_services_; i++) {
        if (services_[i].valid && services_[i].major_channel == major &&
            services_[i].minor_channel == minor) {
            return &services_[i];
        }
    }
    return nullptr;
}

bool ServiceCatalog::update_service(const Service& service) {
    if (!service.valid || service.service_id == 0) {
        return false;
    }

    // Find existing or new slot
    Service* slot = find_slot(service.service_id);
    if (slot == nullptr) {
        // Catalog full
        return false;
    }

    bool is_new = !slot->valid;

    // Copy service data
    *slot = service;

    if (is_new) {
        num_services_++;
    }

    // Notify observers
    if (update_callback_) {
        update_callback_();
    }

    return true;
}

bool ServiceCatalog::remove_service(uint16_t service_id) {
    for (size_t i = 0; i < num_services_; i++) {
        if (services_[i].valid && services_[i].service_id == service_id) {
            // Move last service to this slot
            if (i < num_services_ - 1) {
                services_[i] = services_[num_services_ - 1];
            }
            services_[num_services_ - 1].valid = false;
            num_services_--;

            if (update_callback_) {
                update_callback_();
            }
            return true;
        }
    }
    return false;
}

void ServiceCatalog::clear() {
    for (auto& service : services_) {
        service.valid = false;
        service.service_id = 0;
        service.num_components = 0;
    }
    num_services_ = 0;
}

void ServiceCatalog::set_update_callback(CatalogUpdateCallback callback) {
    update_callback_ = std::move(callback);
}

std::vector<const Service*> ServiceCatalog::get_all_services() const {
    std::vector<const Service*> result;
    result.reserve(num_services_);

    for (size_t i = 0; i < num_services_; i++) {
        if (services_[i].valid) {
            result.push_back(&services_[i]);
        }
    }
    return result;
}

Service* ServiceCatalog::find_slot(uint16_t service_id) {
    // First try to find existing service
    for (size_t i = 0; i < MAX_SERVICES; i++) {
        if (services_[i].valid && services_[i].service_id == service_id) {
            return &services_[i];
        }
    }

    // Find first empty slot
    for (size_t i = 0; i < MAX_SERVICES; i++) {
        if (!services_[i].valid) {
            return &services_[i];
        }
    }

    return nullptr;
}

}  // namespace framing
}  // namespace atsc3
