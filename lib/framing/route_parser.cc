// route_parser.cc — ROUTE/DASH Parser implementation
//
// Parses ATSC 3.0 ROUTE signaling for service discovery

#include "route_parser.h"

#include <algorithm>
#include <cstring>

namespace atsc3 {
namespace framing {

RouteParser::RouteParser(ServiceCatalog* catalog, const RouteParserConfig& config)
    : catalog_(catalog), config_(config) {
    reassembly_buffer_.reserve(config_.max_xml_size);
}

RouteParser::~RouteParser() = default;

bool RouteParser::process_signaling(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return false;
    }

    // Signaling packets contain LCT headers followed by payload
    return process_lct_packet(data, len);
}

bool RouteParser::process_lct_packet(const uint8_t* data, size_t len) {
    if (data == nullptr || len < 4) {
        return false;
    }

    LctHeader header;
    if (!parse_lct_header(data, len, header)) {
        stats_.parse_errors++;
        return false;
    }

    stats_.lct_packets_parsed++;

    // Get payload
    if (header.header_bytes >= len) {
        return false;
    }
    const uint8_t* payload = data + header.header_bytes;
    size_t payload_len = len - header.header_bytes;

    // Handle based on TOI (Transport Object Identifier)
    // Well-known TOIs for signaling
    if (header.toi == route_toi::SLT || header.codepoint == route_codepoint::LLS) {
        return parse_slt(payload, payload_len);
    } else if (header.toi == route_toi::STSID) {
        // S-TSID needs service context - use TSI to find service
        uint16_t service_id = static_cast<uint16_t>(header.tsi & 0xFFFF);
        return parse_stsid(payload, payload_len, service_id);
    } else if (header.toi == route_toi::MPD) {
        uint16_t service_id = static_cast<uint16_t>(header.tsi & 0xFFFF);
        return parse_mpd(payload, payload_len, service_id);
    }

    return false;
}

void RouteParser::set_segment_callback(SegmentCallback callback) {
    segment_callback_ = std::move(callback);
}

const MpdInfo* RouteParser::get_mpd(uint16_t service_id) const {
    for (const auto& pair : mpd_cache_) {
        if (pair.first == service_id) {
            return &pair.second;
        }
    }
    return nullptr;
}

void RouteParser::reset() {
    current_slt_ = SltTable();
    mpd_cache_.clear();
    reassembly_buffer_.clear();
    current_toi_ = 0;
    expected_length_ = 0;
    stats_ = RouteParserStats();
}

bool RouteParser::parse_lct_header(const uint8_t* data, size_t len, LctHeader& header) const {
    if (len < 4) {
        return false;
    }

    // LCT header format (RFC 5651):
    // Byte 0: Version (4 bits) + C (2 bits) + PSI (2 bits)
    // Byte 1: S (1 bit) + O (2 bits) + H (1 bit) + Res (2 bits) + A (1 bit) + B (1 bit)
    // Bytes 2-3: HDR_LEN (8 bits) + CP (8 bits)

    uint8_t byte0 = data[0];
    uint8_t byte1 = data[1];

    header.version = (byte0 >> 4) & 0x0F;
    if (header.version != 1) {
        // Only LCT version 1 supported
        return false;
    }

    header.congestion_control = (byte0 >> 2) & 0x03;
    header.psr = byte0 & 0x03;

    header.transport_session_id_flag = (byte1 >> 7) & 0x01;
    header.transport_object_id_flag = (byte1 >> 5) & 0x03;
    header.half_word_flag = (byte1 >> 4) & 0x01;
    header.close_session_flag = (byte1 >> 1) & 0x01;
    header.close_object_flag = byte1 & 0x01;

    header.header_length = data[2];  // In 32-bit words
    header.codepoint = data[3];

    // Calculate minimum header size
    size_t min_header = 4;  // Base header

    // Congestion Control Information (if C > 0)
    if (header.congestion_control > 0) {
        min_header += 4;
    }

    // TSI size based on S and H flags
    size_t tsi_size = 0;
    if (header.transport_session_id_flag) {
        tsi_size = header.half_word_flag ? 2 : 4;
    }
    min_header += tsi_size;

    // TOI size based on O and H flags
    size_t toi_size = 0;
    switch (header.transport_object_id_flag) {
        case 0:
            toi_size = 0;
            break;
        case 1:
            toi_size = header.half_word_flag ? 2 : 4;
            break;
        case 2:
            toi_size = header.half_word_flag ? 4 : 8;
            break;
        case 3:
            toi_size = header.half_word_flag ? 6 : 12;
            break;
    }
    min_header += toi_size;

    if (len < min_header) {
        return false;
    }

    // Parse variable fields
    size_t offset = 4;

    // CCI
    if (header.congestion_control > 0) {
        header.congestion_control_info = (static_cast<uint32_t>(data[offset]) << 24) |
                                         (static_cast<uint32_t>(data[offset + 1]) << 16) |
                                         (static_cast<uint32_t>(data[offset + 2]) << 8) |
                                         static_cast<uint32_t>(data[offset + 3]);
        offset += 4;
    }

    // TSI
    if (header.transport_session_id_flag) {
        if (header.half_word_flag) {
            header.tsi = (static_cast<uint32_t>(data[offset]) << 8) |
                         static_cast<uint32_t>(data[offset + 1]);
            offset += 2;
        } else {
            header.tsi = (static_cast<uint32_t>(data[offset]) << 24) |
                         (static_cast<uint32_t>(data[offset + 1]) << 16) |
                         (static_cast<uint32_t>(data[offset + 2]) << 8) |
                         static_cast<uint32_t>(data[offset + 3]);
            offset += 4;
        }
    }

    // TOI
    if (header.transport_object_id_flag > 0) {
        switch (header.transport_object_id_flag) {
            case 1:
                if (header.half_word_flag) {
                    header.toi = (static_cast<uint32_t>(data[offset]) << 8) |
                                 static_cast<uint32_t>(data[offset + 1]);
                    offset += 2;
                } else {
                    header.toi = (static_cast<uint32_t>(data[offset]) << 24) |
                                 (static_cast<uint32_t>(data[offset + 1]) << 16) |
                                 (static_cast<uint32_t>(data[offset + 2]) << 8) |
                                 static_cast<uint32_t>(data[offset + 3]);
                    offset += 4;
                }
                break;
            case 2:
            case 3:
                // For longer TOI formats, just read 32 bits for now
                header.toi = (static_cast<uint32_t>(data[offset]) << 24) |
                             (static_cast<uint32_t>(data[offset + 1]) << 16) |
                             (static_cast<uint32_t>(data[offset + 2]) << 8) |
                             static_cast<uint32_t>(data[offset + 3]);
                offset += toi_size;
                break;
        }
    }

    // Total header length (from HDR_LEN field, in 32-bit words)
    header.header_bytes = static_cast<size_t>(header.header_length) * 4;
    if (header.header_bytes < offset) {
        header.header_bytes = offset;  // Use calculated minimum
    }

    return true;
}

bool RouteParser::parse_slt(const uint8_t* data, size_t len) {
    if (data == nullptr || len < 10) {
        return false;
    }

    // Ensure null-terminated for string operations
    std::string xml_str(reinterpret_cast<const char*>(data), len);
    const char* xml = xml_str.c_str();

    // Basic XML validation - look for SLT root element
    const char* slt_start = find_tag(xml, "SLT");
    if (slt_start == nullptr) {
        stats_.xml_errors++;
        return false;
    }

    SltTable slt;

    // Parse BSID attribute
    std::string bsid_str = get_xml_attr(slt_start, "bsid");
    if (!bsid_str.empty()) {
        slt.bsid = static_cast<uint16_t>(std::stoul(bsid_str));
    }

    // Parse version
    std::string version_str = get_xml_attr(slt_start, "version");
    if (!version_str.empty()) {
        slt.version = static_cast<uint8_t>(std::stoul(version_str));
    }

    // Find Service elements
    const char* service_start = slt_start;
    while ((service_start = find_tag(service_start + 1, "Service")) != nullptr) {
        SltService svc;

        // Service ID (required)
        std::string svc_id = get_xml_attr(service_start, "serviceId");
        if (svc_id.empty()) {
            continue;
        }
        svc.service_id = static_cast<uint16_t>(std::stoul(svc_id));

        // Channel numbers
        std::string major = get_xml_attr(service_start, "majorChannelNo");
        std::string minor = get_xml_attr(service_start, "minorChannelNo");
        if (!major.empty()) {
            svc.major_channel = static_cast<uint16_t>(std::stoul(major));
        }
        if (!minor.empty()) {
            svc.minor_channel = static_cast<uint16_t>(std::stoul(minor));
        }

        // Service category
        std::string category = get_xml_attr(service_start, "serviceCategory");
        if (!category.empty()) {
            svc.service_category = static_cast<uint8_t>(std::stoul(category));
        }

        // Short name
        std::string short_name = get_xml_attr(service_start, "shortServiceName");
        if (!short_name.empty()) {
            size_t copy_len = std::min(short_name.size(), MAX_SERVICE_NAME_LEN - 1);
            std::memcpy(svc.short_name.data(), short_name.c_str(), copy_len);
            svc.short_name[copy_len] = '\0';
        }

        // Look for BroadcastSvcSignaling element with ROUTE info
        const char* bss = find_tag(service_start, "BroadcastSvcSignaling");
        if (bss != nullptr) {
            std::string dest_ip = get_xml_attr(bss, "slsDestinationIpAddress");
            std::string dest_port = get_xml_attr(bss, "slsDestinationUdpPort");
            std::string src_ip = get_xml_attr(bss, "slsSourceIpAddress");

            if (!src_ip.empty()) {
                size_t copy_len = std::min(src_ip.size(), sizeof(svc.source_ip) - 1);
                std::memcpy(svc.source_ip.data(), src_ip.c_str(), copy_len);
                svc.source_ip[copy_len] = '\0';
            }
            if (!dest_port.empty()) {
                svc.dest_port = static_cast<uint16_t>(std::stoul(dest_port));
            }
        }

        slt.services.push_back(svc);
    }

    slt.valid = true;
    current_slt_ = slt;
    stats_.slt_tables_received++;

    // Update service catalog
    if (catalog_ != nullptr) {
        for (const auto& slt_svc : slt.services) {
            Service service = slt_to_service(slt_svc);
            catalog_->update_service(service);
        }
    }

    return true;
}

bool RouteParser::parse_stsid(const uint8_t* data, size_t len, uint16_t service_id) {
    if (data == nullptr || len < 10) {
        return false;
    }

    // S-TSID parsing is more complex - for now just track receipt
    stats_.stsid_received++;

    // TODO: Full S-TSID parsing for component TSI mapping
    (void)service_id;

    return true;
}

bool RouteParser::parse_mpd(const uint8_t* data, size_t len, uint16_t service_id) {
    if (data == nullptr || len < 10) {
        return false;
    }

    std::string xml_str(reinterpret_cast<const char*>(data), len);
    const char* xml = xml_str.c_str();

    // Look for MPD root element
    const char* mpd_start = find_tag(xml, "MPD");
    if (mpd_start == nullptr) {
        stats_.xml_errors++;
        return false;
    }

    MpdInfo mpd;

    // Parse BaseURL if present
    const char* base_url = find_tag(mpd_start, "BaseURL");
    if (base_url != nullptr) {
        // Find closing tag and extract content
        const char* end = std::strstr(base_url, "</BaseURL>");
        if (end != nullptr) {
            const char* content_start = std::strchr(base_url, '>');
            if (content_start != nullptr) {
                content_start++;
                mpd.base_url = std::string(content_start, end - content_start);
            }
        }
    }

    // TODO: Full MPD parsing for segment templates and timelines
    mpd.valid = true;
    stats_.mpd_received++;

    // Cache MPD
    bool found = false;
    for (auto& pair : mpd_cache_) {
        if (pair.first == service_id) {
            pair.second = mpd;
            found = true;
            break;
        }
    }
    if (!found) {
        mpd_cache_.emplace_back(service_id, mpd);
    }

    return true;
}

Service RouteParser::slt_to_service(const SltService& slt_svc) const {
    Service service;

    service.service_id = slt_svc.service_id;
    service.major_channel = slt_svc.major_channel;
    service.minor_channel = slt_svc.minor_channel;
    service.service_category = slt_svc.service_category;
    service.protected_service = slt_svc.protected_service;
    service.slt_svc_seq_num = slt_svc.slt_svc_seq_num;

    // Copy short name
    std::memcpy(service.short_name.data(), slt_svc.short_name.data(), slt_svc.short_name.size());

    service.valid = true;

    return service;
}

std::string RouteParser::get_xml_attr(const char* xml, const char* attr_name) const {
    if (xml == nullptr || attr_name == nullptr) {
        return "";
    }

    // Find attribute name
    std::string search = std::string(attr_name) + "=\"";
    const char* attr_start = std::strstr(xml, search.c_str());
    if (attr_start == nullptr) {
        // Try single quotes
        search = std::string(attr_name) + "='";
        attr_start = std::strstr(xml, search.c_str());
    }
    if (attr_start == nullptr) {
        return "";
    }

    // Find attribute value
    const char* value_start = attr_start + search.length();
    char quote_char = *(value_start - 1);
    const char* value_end = std::strchr(value_start, quote_char);
    if (value_end == nullptr) {
        return "";
    }

    return std::string(value_start, value_end - value_start);
}

std::string RouteParser::get_xml_element(const char* xml, const char* element_name) const {
    if (xml == nullptr || element_name == nullptr) {
        return "";
    }

    // Find opening tag
    std::string open_tag = std::string("<") + element_name;
    const char* tag_start = std::strstr(xml, open_tag.c_str());
    if (tag_start == nullptr) {
        return "";
    }

    // Find end of opening tag
    const char* content_start = std::strchr(tag_start, '>');
    if (content_start == nullptr) {
        return "";
    }
    content_start++;

    // Find closing tag
    std::string close_tag = std::string("</") + element_name + ">";
    const char* content_end = std::strstr(content_start, close_tag.c_str());
    if (content_end == nullptr) {
        return "";
    }

    return std::string(content_start, content_end - content_start);
}

const char* RouteParser::find_tag(const char* xml, const char* tag_name) const {
    if (xml == nullptr || tag_name == nullptr) {
        return nullptr;
    }

    std::string search = std::string("<") + tag_name;
    const char* found = std::strstr(xml, search.c_str());

    // Verify it's actually a tag (followed by whitespace, >, or /)
    if (found != nullptr) {
        char next = *(found + search.length());
        if (next != ' ' && next != '\t' && next != '\n' && next != '>' && next != '/' &&
            next != '\r') {
            // Not a real tag match, try again
            return find_tag(found + 1, tag_name);
        }
    }

    return found;
}

}  // namespace framing
}  // namespace atsc3
