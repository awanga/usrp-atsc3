// alp_demux.cc — ALP Demultiplexer implementation
//
// ATSC A/330 Link-Layer Protocol packet parsing and IP reassembly

#include "alp_demux.h"

#include <algorithm>
#include <cstring>

namespace atsc3 {
namespace framing {

namespace {

// CRC-32 lookup table (MPEG-2 polynomial 0x04C11DB7)
constexpr uint32_t CRC32_TABLE[256] = {
    0x00000000, 0x04C11DB7, 0x09823B6E, 0x0D4326D9, 0x130476DC, 0x17C56B6B, 0x1A864DB2, 0x1E475005,
    0x2608EDB8, 0x22C9F00F, 0x2F8AD6D6, 0x2B4BCB61, 0x350C9B64, 0x31CD86D3, 0x3C8EA00A, 0x384FBDBD,
    0x4C11DB70, 0x48D0C6C7, 0x4593E01E, 0x4152FDA9, 0x5F15ADAC, 0x5BD4B01B, 0x569796C2, 0x52568B75,
    0x6A1936C8, 0x6ED82B7F, 0x639B0DA6, 0x675A1011, 0x791D4014, 0x7DDC5DA3, 0x709F7B7A, 0x745E66CD,
    0x9823B6E0, 0x9CE2AB57, 0x91A18D8E, 0x95609039, 0x8B27C03C, 0x8FE6DD8B, 0x82A5FB52, 0x8664E6E5,
    0xBE2B5B58, 0xBAEA46EF, 0xB7A96036, 0xB3687D81, 0xAD2F2D84, 0xA9EE3033, 0xA4AD16EA, 0xA06C0B5D,
    0xD4326D90, 0xD0F37027, 0xDDB056FE, 0xD9714B49, 0xC7361B4C, 0xC3F706FB, 0xCEB42022, 0xCA753D95,
    0xF23A8028, 0xF6FB9D9F, 0xFBB8BB46, 0xFF79A6F1, 0xE13EF6F4, 0xE5FFEB43, 0xE8BCCD9A, 0xEC7DD02D,
    0x34867077, 0x30476DC0, 0x3D044B19, 0x39C556AE, 0x278206AB, 0x23431B1C, 0x2E003DC5, 0x2AC12072,
    0x128E9DCF, 0x164F8078, 0x1B0CA6A1, 0x1FCDBB16, 0x018AEB13, 0x054BF6A4, 0x0808D07D, 0x0CC9CDCA,
    0x7897AB07, 0x7C56B6B0, 0x71159069, 0x75D48DDE, 0x6B93DDDB, 0x6F52C06C, 0x6211E6B5, 0x66D0FB02,
    0x5E9F46BF, 0x5A5E5B08, 0x571D7DD1, 0x53DC6066, 0x4D9B3063, 0x495A2DD4, 0x44190B0D, 0x40D816BA,
    0xACA5C697, 0xA864DB20, 0xA527FDF9, 0xA1E6E04E, 0xBFA1B04B, 0xBB60ADFC, 0xB6238B25, 0xB2E29692,
    0x8AAD2B2F, 0x8E6C3698, 0x832F1041, 0x87EE0DF6, 0x99A95DF3, 0x9D684044, 0x902B669D, 0x94EA7B2A,
    0xE0B41DE7, 0xE4750050, 0xE9362689, 0xEDF73B3E, 0xF3B06B3B, 0xF771768C, 0xFA325055, 0xFEF34DE2,
    0xC6BCF05F, 0xC27DEDE8, 0xCF3ECB31, 0xCBFFD686, 0xD5B88683, 0xD1799B34, 0xDC3ABDED, 0xD8FBA05A,
    0x690CE0EE, 0x6DCDFD59, 0x608EDB80, 0x644FC637, 0x7A089632, 0x7EC98B85, 0x738AAD5C, 0x774BB0EB,
    0x4F040D56, 0x4BC510E1, 0x46863638, 0x42472B8F, 0x5C007B8A, 0x58C1663D, 0x558240E4, 0x51435D53,
    0x251D3B9E, 0x21DC2629, 0x2C9F00F0, 0x285E1D47, 0x36194D42, 0x32D850F5, 0x3F9B762C, 0x3B5A6B9B,
    0x0315D626, 0x07D4CB91, 0x0A97ED48, 0x0E56F0FF, 0x1011A0FA, 0x14D0BD4D, 0x19939B94, 0x1D528623,
    0xF12F560E, 0xF5EE4BB9, 0xF8AD6D60, 0xFC6C70D7, 0xE22B20D2, 0xE6EA3D65, 0xEBA91BBC, 0xEF68060B,
    0xD727BBB6, 0xD3E6A601, 0xDEA580D8, 0xDA649D6F, 0xC423CD6A, 0xC0E2D0DD, 0xCDA1F604, 0xC960EBB3,
    0xBD3E8D7E, 0xB9FF90C9, 0xB4BCB610, 0xB07DABA7, 0xAE3AFBA2, 0xAAFBE615, 0xA7B8C0CC, 0xA379DD7B,
    0x9B3660C6, 0x9FF77D71, 0x92B45BA8, 0x9675461F, 0x8832161A, 0x8CF30BAD, 0x81B02D74, 0x857130C3,
    0x5D8A9099, 0x594B8D2E, 0x5408ABF7, 0x50C9B640, 0x4E8EE645, 0x4A4FFBF2, 0x470CDD2B, 0x43CDC09C,
    0x7B827D21, 0x7F436096, 0x7200464F, 0x76C15BF8, 0x68860BFD, 0x6C47164A, 0x61043093, 0x65C52D24,
    0x119B4BE9, 0x155A565E, 0x18197087, 0x1CD86D30, 0x029F3D35, 0x065E2082, 0x0B1D065B, 0x0FDC1BEC,
    0x3793A651, 0x3352BBE6, 0x3E119D3F, 0x3AD08088, 0x2497D08D, 0x2056CD3A, 0x2D15EBE3, 0x29D4F654,
    0xC5A92679, 0xC1683BCE, 0xCC2B1D17, 0xC8EA00A0, 0xD6AD50A5, 0xD26C4D12, 0xDF2F6BCB, 0xDBEE767C,
    0xE3A1CBC1, 0xE760D676, 0xEA23F0AF, 0xEEE2ED18, 0xF0A5BD1D, 0xF464A0AA, 0xF9278673, 0xFDE69BC4,
    0x89B8FD09, 0x8D79E0BE, 0x803AC667, 0x84FBDBD0, 0x9ABC8BD5, 0x9E7D9662, 0x933EB0BB, 0x97FFAD0C,
    0xAFB010B1, 0xAB710D06, 0xA6322BDF, 0xA2F33668, 0xBCB4666D, 0xB8757BDA, 0xB5365D03, 0xB1F740B4};

}  // namespace

AlpDemux::AlpDemux(const AlpDemuxConfig& config) : config_(config) {
    // Pre-allocate input buffer
    input_buffer_.reserve(config_.max_datagram_size);

    // Initialize reassembly contexts
    for (auto& ctx : reassembly_) {
        ctx.buffer.reserve(config_.max_datagram_size);
        ctx.active = false;
    }
}

AlpDemux::~AlpDemux() = default;

size_t AlpDemux::process(uint8_t plp_id, const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return 0;
    }

    current_plp_id_ = plp_id;
    size_t packets_parsed = 0;

    // Append to input buffer
    size_t prev_size = input_buffer_.size();
    input_buffer_.resize(prev_size + len);
    std::memcpy(input_buffer_.data() + prev_size, data, len);

    // Process complete packets
    size_t offset = 0;
    while (offset < input_buffer_.size()) {
        // Need at least minimum header
        if (input_buffer_.size() - offset < alp_fields::MIN_HEADER_BYTES) {
            break;
        }

        // Parse header
        AlpHeader header;
        if (!parse_header(input_buffer_.data() + offset, input_buffer_.size() - offset, header)) {
            // Invalid header - skip one byte and try again
            stats_.header_errors++;
            offset++;
            continue;
        }

        // Check if we have complete packet
        size_t total_packet_size = header.header_bytes + header.length;
        if (offset + total_packet_size > input_buffer_.size()) {
            // Incomplete packet, wait for more data
            break;
        }

        // Process complete packet
        const uint8_t* payload = input_buffer_.data() + offset + header.header_bytes;
        process_packet(plp_id, header, payload, header.length);
        packets_parsed++;
        stats_.packets_received++;
        packet_counter_++;

        offset += total_packet_size;
    }

    // Remove processed data from buffer
    if (offset > 0) {
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + offset);
    }

    // Check for reassembly timeouts
    check_reassembly_timeouts();

    return packets_parsed;
}

void AlpDemux::set_ip_callback(IpDatagramCallback callback) {
    ip_callback_ = std::move(callback);
}

void AlpDemux::set_signaling_callback(SignalingCallback callback) {
    signaling_callback_ = std::move(callback);
}

void AlpDemux::set_ts_callback(TsPacketCallback callback) {
    ts_callback_ = std::move(callback);
}

void AlpDemux::reset_stats() {
    stats_ = AlpStats();
}

void AlpDemux::reset() {
    input_buffer_.clear();
    for (auto& ctx : reassembly_) {
        ctx.buffer.clear();
        ctx.active = false;
        ctx.expected_sequence = 0;
        ctx.total_received = 0;
    }
    packet_counter_ = 0;
    reset_stats();
}

void AlpDemux::flush() {
    // Deliver any complete reassembly contexts
    for (auto& ctx : reassembly_) {
        if (ctx.active && ctx.buffer.size() > 0) {
            // Incomplete, but deliver what we have
            stats_.reassembly_errors++;
            ctx.active = false;
            ctx.buffer.clear();
        }
    }
    input_buffer_.clear();
}

bool AlpDemux::parse_header(const uint8_t* data, size_t len, AlpHeader& header) const {
    if (len < alp_fields::MIN_HEADER_BYTES) {
        return false;
    }

    // Byte 0: packet_type[7:5] + PC[4] + HM/length[3:0]
    uint8_t byte0 = data[0];
    header.packet_type = static_cast<AlpPacketType>((byte0 >> 5) & 0x07);
    header.payload_config = (byte0 >> 4) & 0x01;

    if (header.payload_config == 0) {
        // Single-header mode: length is 11 bits (4 from byte0 + 7 from byte1)
        if (len < 2) {
            return false;
        }
        uint8_t byte1 = data[1];
        header.length = ((byte0 & 0x0F) << 7) | (byte1 >> 1);
        header.header_bytes = 2;
        header.segmentation = false;
        header.concatenation = false;
    } else {
        // Additional header mode
        header.header_mode = (byte0 >> 3) & 0x01;

        if (header.header_mode == 0) {
            // Length extension only
            if (len < 3) {
                return false;
            }
            uint8_t byte1 = data[1];
            uint8_t byte2 = data[2];
            header.length = ((byte0 & 0x07) << 10) | (byte1 << 2) | (byte2 >> 6);
            header.header_bytes = 3;
            header.segmentation = false;
            header.concatenation = false;
        } else {
            // Full additional header with SIF/CIF
            if (len < 3) {
                return false;
            }
            uint8_t byte1 = data[1];
            header.segmentation = (byte1 >> 7) & 0x01;
            header.concatenation = (byte1 >> 6) & 0x01;

            // Length from remaining bits
            uint8_t byte2 = data[2];
            header.length = ((byte1 & 0x3F) << 7) | (byte2 >> 1);
            header.header_bytes = 3;

            // Segmentation header if SIF=1
            if (header.segmentation) {
                if (len < header.header_bytes + 2) {
                    return false;
                }
                uint8_t seg_byte0 = data[header.header_bytes];
                uint8_t seg_byte1 = data[header.header_bytes + 1];

                header.segment_sequence = (seg_byte0 >> 3) & 0x1F;
                header.last_segment = (seg_byte0 >> 2) & 0x01;
                header.sub_stream_id = ((seg_byte0 & 0x03) << 6) | (seg_byte1 >> 2);
                header.header_bytes += 2;
            }
        }
    }

    // Validate packet type
    switch (header.packet_type) {
        case AlpPacketType::IPV4:
        case AlpPacketType::COMPRESSED:
        case AlpPacketType::SIGNALING:
        case AlpPacketType::EXTENSION:
        case AlpPacketType::MPEG2_TS:
            break;
        default:
            // Reserved packet type
            return false;
    }

    // Validate length
    if (header.length > AlpDemuxConfig::MAX_ALP_PACKET_SIZE) {
        return false;
    }

    return true;
}

void AlpDemux::process_packet(uint8_t plp_id, const AlpHeader& header, const uint8_t* payload,
                              size_t payload_len) {
    switch (header.packet_type) {
        case AlpPacketType::IPV4:
        case AlpPacketType::COMPRESSED:
            handle_ipv4_packet(plp_id, header, payload, payload_len);
            break;

        case AlpPacketType::SIGNALING:
            handle_signaling_packet(plp_id, payload, payload_len);
            break;

        case AlpPacketType::MPEG2_TS:
            handle_ts_packet(plp_id, payload, payload_len);
            break;

        case AlpPacketType::EXTENSION:
            // Extension packets - count but don't process
            stats_.extension_packets++;
            break;

        default:
            // Unknown type
            break;
    }
}

void AlpDemux::handle_ipv4_packet(uint8_t plp_id, const AlpHeader& header, const uint8_t* payload,
                                  size_t payload_len) {
    stats_.ipv4_packets++;

    if (!header.segmentation) {
        // Complete IP packet - deliver directly
        if (ip_callback_ && payload_len > 0) {
            // Verify CRC if enabled
            if (config_.check_crc && payload_len >= 4) {
                if (!verify_crc(payload, payload_len)) {
                    stats_.crc_errors++;
                    return;
                }
                // Remove CRC from payload
                payload_len -= 4;
            }
            ip_callback_(plp_id, payload, payload_len);
            stats_.datagrams_reassembled++;
        }
    } else {
        // Fragmented packet - reassemble
        stats_.fragments_received++;

        ReassemblyContext* ctx = nullptr;

        if (header.segment_sequence == 0) {
            // First segment - get new context
            ctx = get_reassembly_context(header.sub_stream_id);
            if (ctx == nullptr) {
                stats_.reassembly_errors++;
                return;
            }
            ctx->buffer.clear();
            ctx->expected_sequence = 0;
            ctx->sub_stream_id = header.sub_stream_id;
            ctx->total_received = 0;
        } else {
            // Continuation segment - find existing context
            ctx = find_reassembly_context(header.sub_stream_id);
            if (ctx == nullptr) {
                // No context for this sub-stream
                stats_.reassembly_errors++;
                return;
            }
        }

        // Check sequence
        if (header.segment_sequence != ctx->expected_sequence) {
            // Out of order or missing segment
            stats_.reassembly_errors++;
            release_reassembly_context(ctx);
            return;
        }

        // Append payload
        if (ctx->buffer.size() + payload_len > config_.max_datagram_size) {
            // Oversized datagram
            stats_.reassembly_errors++;
            release_reassembly_context(ctx);
            return;
        }

        size_t prev_size = ctx->buffer.size();
        ctx->buffer.resize(prev_size + payload_len);
        std::memcpy(ctx->buffer.data() + prev_size, payload, payload_len);
        ctx->expected_sequence++;
        ctx->total_received += payload_len;

        if (header.last_segment) {
            // Reassembly complete
            complete_reassembly(plp_id, ctx);
        }
    }
}

void AlpDemux::handle_signaling_packet(uint8_t plp_id, const uint8_t* payload, size_t payload_len) {
    stats_.signaling_packets++;

    if (signaling_callback_ && payload_len > 0) {
        signaling_callback_(plp_id, payload, payload_len);
    }
}

void AlpDemux::handle_ts_packet(uint8_t plp_id, const uint8_t* payload, size_t payload_len) {
    stats_.ts_packets++;

    // MPEG-2 TS packets are always 188 bytes
    if (ts_callback_ && payload_len >= alp_fields::TS_PACKET_SIZE) {
        // Deliver complete TS packets
        size_t offset = 0;
        while (offset + alp_fields::TS_PACKET_SIZE <= payload_len) {
            ts_callback_(plp_id, payload + offset, alp_fields::TS_PACKET_SIZE);
            offset += alp_fields::TS_PACKET_SIZE;
        }
    }
}

ReassemblyContext* AlpDemux::get_reassembly_context(uint8_t sub_stream_id) {
    // First check if we already have a context for this sub-stream
    ReassemblyContext* ctx = find_reassembly_context(sub_stream_id);
    if (ctx != nullptr) {
        return ctx;
    }

    // Find free context
    for (auto& reassembly_ctx : reassembly_) {
        if (!reassembly_ctx.active) {
            reassembly_ctx.active = true;
            reassembly_ctx.sub_stream_id = sub_stream_id;
            reassembly_ctx.buffer.clear();
            reassembly_ctx.expected_sequence = 0;
            reassembly_ctx.total_received = 0;
            return &reassembly_ctx;
        }
    }

    // No free contexts
    return nullptr;
}

ReassemblyContext* AlpDemux::find_reassembly_context(uint8_t sub_stream_id) {
    for (auto& ctx : reassembly_) {
        if (ctx.active && ctx.sub_stream_id == sub_stream_id) {
            return &ctx;
        }
    }
    return nullptr;
}

void AlpDemux::release_reassembly_context(ReassemblyContext* ctx) {
    if (ctx != nullptr) {
        ctx->active = false;
        ctx->buffer.clear();
        ctx->expected_sequence = 0;
        ctx->total_received = 0;
    }
}

void AlpDemux::complete_reassembly(uint8_t plp_id, ReassemblyContext* ctx) {
    if (ctx == nullptr || ctx->buffer.empty()) {
        return;
    }

    // Verify CRC if enabled
    if (config_.check_crc && ctx->buffer.size() >= 4) {
        if (!verify_crc(ctx->buffer.data(), ctx->buffer.size())) {
            stats_.crc_errors++;
            release_reassembly_context(ctx);
            return;
        }
        // Remove CRC from buffer
        ctx->buffer.resize(ctx->buffer.size() - 4);
    }

    // Deliver complete datagram
    if (ip_callback_ && ctx->buffer.size() > 0) {
        ip_callback_(plp_id, ctx->buffer.data(), ctx->buffer.size());
        stats_.datagrams_reassembled++;
    }

    release_reassembly_context(ctx);
}

void AlpDemux::check_reassembly_timeouts() {
    // Simple timeout: if context hasn't been updated in a while, release it
    // This is a simplistic approach - real implementation would track timestamps
    static uint64_t last_check = 0;

    if (packet_counter_ - last_check < config_.reassembly_timeout_packets / 10) {
        return;
    }
    last_check = packet_counter_;

    // For now, we don't implement timeout based on packet count
    // A real implementation would track last-update time per context
}

bool AlpDemux::verify_crc(const uint8_t* data, size_t len) const {
    if (len < 4) {
        return false;
    }

    // Compute CRC over data excluding last 4 bytes
    uint32_t computed = compute_crc32(data, len - 4);

    // Extract transmitted CRC (big-endian)
    uint32_t transmitted = (static_cast<uint32_t>(data[len - 4]) << 24) |
                           (static_cast<uint32_t>(data[len - 3]) << 16) |
                           (static_cast<uint32_t>(data[len - 2]) << 8) |
                           static_cast<uint32_t>(data[len - 1]);

    return computed == transmitted;
}

uint32_t AlpDemux::compute_crc32(const uint8_t* data, size_t len) const {
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < len; i++) {
        uint8_t idx = (crc >> 24) ^ data[i];
        crc = (crc << 8) ^ CRC32_TABLE[idx];
    }

    return crc;
}

}  // namespace framing
}  // namespace atsc3
