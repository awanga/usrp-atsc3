#pragma once

// Service Catalog — ATSC 3.0 Service Discovery
//
// Maintains list of available broadcast services discovered from
// SLT (Service List Table) and ROUTE session announcements.
//
// Each service contains:
//   - Service ID and name
//   - Audio/video component info
//   - ROUTE session parameters for content delivery
//
// Reference: ATSC A/331 (Signaling, Delivery, and Synchronization)

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace atsc3 {
namespace framing {

// Maximum limits for fixed-size arrays (RTL portability)
constexpr size_t MAX_SERVICES = 32;
constexpr size_t MAX_COMPONENTS_PER_SERVICE = 8;
constexpr size_t MAX_SERVICE_NAME_LEN = 64;
constexpr size_t MAX_LANGUAGE_LEN = 8;

// Component types per ATSC A/331
enum class ComponentType : uint8_t {
    UNKNOWN = 0,
    VIDEO = 1,  // HEVC video
    AUDIO = 2,  // AC-4 or AAC audio
    CLOSED_CAPTION = 3,
    APPLICATION = 4,  // Application data (HTML5, etc.)
    ESG = 5,          // Electronic service guide
    EAS = 6           // Emergency alert
};

// Video codec
enum class VideoCodec : uint8_t {
    UNKNOWN = 0,
    HEVC = 1,  // H.265/HEVC
    AVC = 2,   // H.264/AVC (legacy)
    VVC = 3    // H.266/VVC (future)
};

// Audio codec
enum class AudioCodec : uint8_t {
    UNKNOWN = 0,
    AC4 = 1,    // Dolby AC-4
    AAC = 2,    // HE-AAC v2
    MPEG_H = 3  // MPEG-H 3D Audio
};

// Service component (audio, video, caption track)
struct ServiceComponent {
    ComponentType type = ComponentType::UNKNOWN;
    uint8_t component_id = 0;

    // For video components
    VideoCodec video_codec = VideoCodec::UNKNOWN;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t frame_rate = 0;  // FPS

    // For audio components
    AudioCodec audio_codec = AudioCodec::UNKNOWN;
    uint8_t channels = 0;
    uint32_t sample_rate = 0;
    std::array<char, MAX_LANGUAGE_LEN> language = {};

    // ROUTE delivery parameters
    uint32_t tsi = 0;  // Transport Session Identifier
    uint16_t source_flow_id = 0;

    bool valid = false;
};

// Broadcast service
struct Service {
    // Service identification
    uint16_t service_id = 0;
    uint16_t major_channel = 0;
    uint16_t minor_channel = 0;
    std::array<char, MAX_SERVICE_NAME_LEN> short_name = {};
    std::array<char, MAX_SERVICE_NAME_LEN> long_name = {};

    // Service category
    uint8_t service_category = 0;  // 1=TV, 2=Radio, 3=App

    // Emergency alert capable
    bool ea_capable = false;

    // Components (video, audio, captions)
    std::array<ServiceComponent, MAX_COMPONENTS_PER_SERVICE> components;
    size_t num_components = 0;

    // ROUTE session info
    uint32_t slt_svc_seq_num = 0;    // SLT service sequence number
    bool protected_service = false;  // Requires CA

    // Validity
    bool valid = false;

    // Helper to find video component
    const ServiceComponent* get_video() const {
        for (size_t i = 0; i < num_components; i++) {
            if (components[i].type == ComponentType::VIDEO && components[i].valid) {
                return &components[i];
            }
        }
        return nullptr;
    }

    // Helper to find primary audio component
    const ServiceComponent* get_audio() const {
        for (size_t i = 0; i < num_components; i++) {
            if (components[i].type == ComponentType::AUDIO && components[i].valid) {
                return &components[i];
            }
        }
        return nullptr;
    }
};

// ROUTE session parameters
struct RouteSession {
    uint32_t tsi = 0;                     // Transport Session Identifier
    std::array<char, 64> source_ip = {};  // Source IP address (multicast)
    uint16_t source_port = 0;
    uint16_t dest_port = 0;

    // Session description
    uint8_t codepoint = 0;  // Payload format
    uint32_t sender_current_time = 0;

    bool valid = false;
};

// Callback for service updates
using ServiceUpdateCallback = std::function<void(const Service&)>;
using CatalogUpdateCallback = std::function<void()>;

// Service Catalog
//
// Maintains discovered broadcast services from SLT parsing.
// Services are updated as new signaling is received.
//
// Usage:
//   ServiceCatalog catalog;
//
//   // Register for updates
//   catalog.set_update_callback([](){ /* refresh UI */ });
//
//   // Services are populated by RouteParser
//   for (size_t i = 0; i < catalog.service_count(); i++) {
//       const auto* svc = catalog.get_service(i);
//       // Display service info
//   }
//
class ServiceCatalog {
public:
    ServiceCatalog();
    ~ServiceCatalog();

    // Get service by index (0 to service_count()-1)
    const Service* get_service(size_t index) const;

    // Get service by service_id
    const Service* find_service(uint16_t service_id) const;

    // Get service by channel number
    const Service* find_by_channel(uint16_t major, uint16_t minor) const;

    // Number of services in catalog
    size_t service_count() const {
        return num_services_;
    }

    // Add or update service
    // Returns true if service was added/updated
    bool update_service(const Service& service);

    // Remove service by ID
    bool remove_service(uint16_t service_id);

    // Clear all services
    void clear();

    // Register callback for catalog updates
    void set_update_callback(CatalogUpdateCallback callback);

    // Get all services as vector (for iteration)
    std::vector<const Service*> get_all_services() const;

private:
    std::array<Service, MAX_SERVICES> services_;
    size_t num_services_ = 0;
    CatalogUpdateCallback update_callback_;

    // Find slot for service (existing or new)
    Service* find_slot(uint16_t service_id);
};

}  // namespace framing
}  // namespace atsc3
