#pragma once

// ROUTE/DASH Parser — ATSC 3.0 Content Delivery
//
// Parses ROUTE (Real-time Object delivery over Unidirectional Transport)
// signaling to discover broadcast services and resolve content URLs.
//
// Key signaling tables:
//   - SLT (Service List Table): Available services per broadcast
//   - S-TSID (Service-based Transport Session Instance Description)
//   - MPD (Media Presentation Description): DASH manifest for A/V segments
//
// AXI4-S Interface Contract:
//   Input:  TDATA=uint8_t (signaling packets from ALP demux)
//   Output: ServiceCatalog updates
//           DASH segment URLs via callback
//
// Reference: ATSC A/331 (Signaling, Delivery, and Synchronization)

#include "service_catalog.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace atsc3 {
namespace framing {

// LCT (Layered Coding Transport) header structure
struct LctHeader {
    uint8_t version = 0;  // LCT version (should be 1)
    uint8_t congestion_control = 0;
    uint8_t psr = 0;  // Protocol-specific indication
    uint8_t transport_session_id_flag = 0;
    uint8_t transport_object_id_flag = 0;
    uint8_t half_word_flag = 0;
    uint8_t close_session_flag = 0;
    uint8_t close_object_flag = 0;

    uint16_t header_length = 0;  // In 32-bit words
    uint16_t codepoint = 0;      // Payload format ID

    uint32_t congestion_control_info = 0;
    uint32_t tsi = 0;  // Transport Session Identifier
    uint32_t toi = 0;  // Transport Object Identifier

    // Sender current time
    bool sct_present = false;
    uint32_t sender_current_time = 0;

    // Expected transfer length
    bool elt_present = false;
    uint64_t expected_transfer_length = 0;

    size_t header_bytes = 0;  // Total header size for offset
};

// SLT service entry
struct SltService {
    uint16_t service_id = 0;
    uint8_t slt_svc_seq_num = 0;
    bool protected_service = false;
    uint16_t major_channel = 0;
    uint16_t minor_channel = 0;
    uint8_t service_category = 0;
    std::array<char, MAX_SERVICE_NAME_LEN> short_name = {};
    bool hidden = false;
    bool broadcast_live = false;

    // Source IP for ROUTE session
    std::array<char, 64> source_ip = {};
    uint16_t dest_port = 0;
    uint32_t tsi = 0;
};

// Parsed SLT table
struct SltTable {
    uint8_t version = 0;
    uint16_t bsid = 0;  // Broadcast Stream ID
    std::vector<SltService> services;
    bool valid = false;
};

// DASH segment info
struct DashSegment {
    uint32_t toi = 0;  // Transport Object Identifier
    std::string url;   // Segment URL (relative or absolute)
    uint64_t byte_offset = 0;
    uint64_t byte_length = 0;
    uint32_t presentation_time = 0;  // In timescale units
    uint32_t duration = 0;           // In timescale units
};

// MPD (Media Presentation Description) info
struct MpdInfo {
    std::string base_url;
    uint32_t timescale = 1;
    uint64_t presentation_time_offset = 0;

    // Current segment info
    std::vector<DashSegment> video_segments;
    std::vector<DashSegment> audio_segments;

    bool valid = false;
};

// Parser statistics
struct RouteParserStats {
    uint64_t lct_packets_parsed = 0;
    uint64_t slt_tables_received = 0;
    uint64_t stsid_received = 0;
    uint64_t mpd_received = 0;
    uint64_t parse_errors = 0;
    uint64_t xml_errors = 0;
};

// Parser configuration
struct RouteParserConfig {
    // Maximum XML document size to parse
    size_t max_xml_size = 1024 * 1024;  // 1MB

    // Enable verbose logging
    bool verbose = false;
};

// Callback for new DASH segments
using SegmentCallback = std::function<void(uint16_t service_id, const DashSegment& segment)>;

// ROUTE Parser
//
// Parses ROUTE signaling from ALP signaling packets.
// Updates ServiceCatalog with discovered services.
// Provides DASH segment URLs for A/V content delivery.
//
// Usage:
//   ServiceCatalog catalog;
//   RouteParser parser(&catalog);
//
//   // Register for segment notifications
//   parser.set_segment_callback([](uint16_t svc, const DashSegment& seg) {
//       // Fetch segment from broadcast
//   });
//
//   // Feed signaling packets (from ALP demux)
//   parser.process_signaling(data, len);
//
class RouteParser {
public:
    explicit RouteParser(ServiceCatalog* catalog,
                         const RouteParserConfig& config = RouteParserConfig());
    ~RouteParser();

    // Process signaling data from ALP demux
    // Returns true if any signaling was parsed
    bool process_signaling(const uint8_t* data, size_t len);

    // Process LCT packet directly
    bool process_lct_packet(const uint8_t* data, size_t len);

    // Register callback for segment availability
    void set_segment_callback(SegmentCallback callback);

    // Get current SLT
    const SltTable& get_slt() const {
        return current_slt_;
    }

    // Get MPD for a service
    const MpdInfo* get_mpd(uint16_t service_id) const;

    // Get statistics
    const RouteParserStats& get_stats() const {
        return stats_;
    }

    // Reset parser state
    void reset();

private:
    ServiceCatalog* catalog_;
    RouteParserConfig config_;
    RouteParserStats stats_;

    SltTable current_slt_;
    std::vector<std::pair<uint16_t, MpdInfo>> mpd_cache_;

    SegmentCallback segment_callback_;

    // Object reassembly buffer (for multi-packet objects)
    std::vector<uint8_t> reassembly_buffer_;
    uint32_t current_toi_ = 0;
    uint64_t expected_length_ = 0;

    // Parse LCT header
    bool parse_lct_header(const uint8_t* data, size_t len, LctHeader& header) const;

    // Parse SLT XML
    bool parse_slt(const uint8_t* data, size_t len);

    // Parse S-TSID XML
    bool parse_stsid(const uint8_t* data, size_t len, uint16_t service_id);

    // Parse MPD XML
    bool parse_mpd(const uint8_t* data, size_t len, uint16_t service_id);

    // Convert SLT service to catalog Service
    Service slt_to_service(const SltService& slt_svc) const;

    // XML helper: extract attribute value
    std::string get_xml_attr(const char* xml, const char* attr_name) const;

    // XML helper: extract element text content
    std::string get_xml_element(const char* xml, const char* element_name) const;

    // XML helper: find tag start
    const char* find_tag(const char* xml, const char* tag_name) const;
};

// Well-known ROUTE codepoints
namespace route_codepoint {
constexpr uint8_t LLS = 0;        // Low-level signaling (SLT, etc.)
constexpr uint8_t SLS = 1;        // Service-level signaling (S-TSID, MPD)
constexpr uint8_t AV_STREAM = 2;  // A/V media segments
constexpr uint8_t NRT = 3;        // Non-real-time content
}  // namespace route_codepoint

// Well-known TOI values
namespace route_toi {
constexpr uint32_t SLT = 0xFFFFFF00;
constexpr uint32_t STSID = 0xFFFFFF01;
constexpr uint32_t MPD = 0xFFFFFF02;
}  // namespace route_toi

}  // namespace framing
}  // namespace atsc3
