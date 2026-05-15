#pragma once

// ALP Demultiplexer — ATSC 3.0 Link-Layer Protocol
//
// Parses ALP packets per ATSC A/330 and reassembles IP datagrams.
// Handles packet types:
//   - Type 0: IPv4 packet
//   - Type 2: Compressed IP header
//   - Type 4: Link layer signaling
//   - Type 6: Packet type extension
//   - Type 7: MPEG-2 TS
//
// Supports fragmentation/reassembly of oversized IP datagrams.
// Demultiplexes based on PLP ID (from L1 signaling).
//
// AXI4-S Interface Contract:
//   Input:  TDATA=uint8_t (byte stream after FEC decode)
//           TLAST=FEC block boundary
//   Output: Reassembled IP datagrams via callback
//           Per-PLP byte streams
//
// Reference: ATSC A/330 Section 5 (ALP)

#include "config/atsc3_config.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace atsc3 {
namespace framing {

// ALP packet types per ATSC A/330 §5.1.1
enum class AlpPacketType : uint8_t {
    IPV4 = 0,        // IPv4 packet (uncompressed)
    RESERVED_1 = 1,  // Reserved
    COMPRESSED = 2,  // Compressed IP packet
    RESERVED_3 = 3,  // Reserved
    SIGNALING = 4,   // Link layer signaling
    RESERVED_5 = 5,  // Reserved
    EXTENSION = 6,   // Packet type extension
    MPEG2_TS = 7     // MPEG-2 TS packet
};

// ALP header structure (parsed from packet)
struct AlpHeader {
    AlpPacketType packet_type = AlpPacketType::IPV4;
    bool payload_config = false;  // 0=single, 1=header mode
    bool header_mode = false;     // Header mode indicator (if payload_config=1)
    uint16_t length = 0;          // Payload length in bytes

    // Segmentation/concatenation (SIF) fields
    bool segmentation = false;   // Segment indicator flag
    bool concatenation = false;  // Concatenation indicator flag

    // For segmented packets
    uint8_t segment_sequence = 0;  // Sequence number within fragmented IP
    bool last_segment = false;     // Last segment indicator

    // Additional ID for reassembly context
    uint8_t sub_stream_id = 0;  // Sub-stream identification (SID)

    // Header length (for offset calculation)
    size_t header_bytes = 0;
};

// Reassembly context for fragmented IP datagrams
struct ReassemblyContext {
    std::vector<uint8_t> buffer;    // Reassembly buffer
    uint8_t expected_sequence = 0;  // Next expected segment
    uint8_t sub_stream_id = 0;      // Stream identifier
    bool active = false;            // Context in use
    size_t total_received = 0;      // Bytes received so far
};

// ALP statistics
struct AlpStats {
    uint64_t packets_received = 0;       // Total ALP packets parsed
    uint64_t ipv4_packets = 0;           // IPv4 packets
    uint64_t signaling_packets = 0;      // Signaling packets
    uint64_t extension_packets = 0;      // Extension packets
    uint64_t ts_packets = 0;             // MPEG-2 TS packets
    uint64_t fragments_received = 0;     // IP fragments
    uint64_t datagrams_reassembled = 0;  // Complete IP datagrams
    uint64_t reassembly_errors = 0;      // Reassembly failures (missing/duplicate)
    uint64_t crc_errors = 0;             // CRC validation failures
    uint64_t header_errors = 0;          // Invalid header errors
};

// Configuration for ALP demultiplexer
struct AlpDemuxConfig {
    // Maximum IP datagram size (default 64KB per IP spec)
    size_t max_datagram_size = 65535;

    // Maximum reassembly contexts (concurrent fragmented streams)
    static constexpr size_t MAX_REASSEMBLY_CONTEXTS = 16;

    // Enable CRC checking (can be disabled for testing)
    bool check_crc = true;

    // Timeout for incomplete reassembly (in packets processed)
    // After this many packets without completion, context is reset
    size_t reassembly_timeout_packets = 1000;

    // Maximum single ALP packet length
    static constexpr size_t MAX_ALP_PACKET_SIZE = 65535;
};

// Callback types for demultiplexed output
using IpDatagramCallback = std::function<void(uint8_t plp_id, const uint8_t* data, size_t len)>;
using SignalingCallback = std::function<void(uint8_t plp_id, const uint8_t* data, size_t len)>;
using TsPacketCallback = std::function<void(uint8_t plp_id, const uint8_t* data, size_t len)>;

// ALP Demultiplexer
//
// Parses ALP packets from baseband byte stream and outputs:
//   - Reassembled IP datagrams (for ROUTE/DASH)
//   - Signaling information
//   - MPEG-2 TS packets (if present)
//
// Usage:
//   AlpDemux demux;
//
//   // Register callbacks
//   demux.set_ip_callback([](uint8_t plp, const uint8_t* data, size_t len) {
//       // Process IP datagram
//   });
//
//   // Process FEC-decoded bytes
//   demux.process(plp_id, fec_output, num_bytes);
//
class AlpDemux {
public:
    explicit AlpDemux(const AlpDemuxConfig& config = AlpDemuxConfig());
    ~AlpDemux();

    // Process incoming byte stream from FEC decoder
    // plp_id: Physical Layer Pipe identifier (from L1 signaling)
    // data: Decoded bytes after LDPC+BCH
    // len: Number of bytes
    // Returns: Number of complete ALP packets parsed
    size_t process(uint8_t plp_id, const uint8_t* data, size_t len);

    // Register callback for reassembled IP datagrams
    void set_ip_callback(IpDatagramCallback callback);

    // Register callback for signaling packets
    void set_signaling_callback(SignalingCallback callback);

    // Register callback for MPEG-2 TS packets
    void set_ts_callback(TsPacketCallback callback);

    // Get statistics
    const AlpStats& get_stats() const {
        return stats_;
    }

    // Reset statistics
    void reset_stats();

    // Reset all state (reassembly contexts, buffers)
    void reset();

    // Flush any pending reassembly (for end of stream)
    void flush();

    // Get configuration
    const AlpDemuxConfig& get_config() const {
        return config_;
    }

private:
    AlpDemuxConfig config_;
    AlpStats stats_;

    // Callbacks
    IpDatagramCallback ip_callback_;
    SignalingCallback signaling_callback_;
    TsPacketCallback ts_callback_;

    // Reassembly contexts (one per sub-stream)
    std::array<ReassemblyContext, AlpDemuxConfig::MAX_REASSEMBLY_CONTEXTS> reassembly_;

    // Input buffer for partial packets
    std::vector<uint8_t> input_buffer_;
    uint8_t current_plp_id_ = 0;

    // Packet counter for reassembly timeout
    uint64_t packet_counter_ = 0;

    // Parse ALP header from buffer
    // Returns true if valid header parsed
    bool parse_header(const uint8_t* data, size_t len, AlpHeader& header) const;

    // Process a single complete ALP packet
    void process_packet(uint8_t plp_id, const AlpHeader& header, const uint8_t* payload,
                        size_t payload_len);

    // Handle IPv4 packet (possibly fragmented)
    void handle_ipv4_packet(uint8_t plp_id, const AlpHeader& header, const uint8_t* payload,
                            size_t payload_len);

    // Handle signaling packet
    void handle_signaling_packet(uint8_t plp_id, const uint8_t* payload, size_t payload_len);

    // Handle MPEG-2 TS packet
    void handle_ts_packet(uint8_t plp_id, const uint8_t* payload, size_t payload_len);

    // Get or create reassembly context for given sub-stream
    ReassemblyContext* get_reassembly_context(uint8_t sub_stream_id);

    // Find existing reassembly context
    ReassemblyContext* find_reassembly_context(uint8_t sub_stream_id);

    // Release reassembly context
    void release_reassembly_context(ReassemblyContext* ctx);

    // Complete reassembly and deliver datagram
    void complete_reassembly(uint8_t plp_id, ReassemblyContext* ctx);

    // Check for reassembly timeouts
    void check_reassembly_timeouts();

    // Verify CRC-32 (if enabled)
    bool verify_crc(const uint8_t* data, size_t len) const;

    // CRC-32 computation (MPEG-2 variant)
    uint32_t compute_crc32(const uint8_t* data, size_t len) const;
};

// ALP header field definitions per ATSC A/330 §5.1
namespace alp_fields {
// Byte 0: packet_type (3 bits) + payload_config (1 bit) + header/length (4 bits)
constexpr size_t PACKET_TYPE_BITS = 3;
constexpr size_t PAYLOAD_CONFIG_BITS = 1;
constexpr size_t HEADER_MODE_BITS = 1;
constexpr size_t LENGTH_MSB_BITS = 4;

// Single-header mode (PC=0): length is 11 bits total
constexpr size_t SINGLE_LENGTH_BITS = 11;

// Additional header mode (PC=1, HM=1): segmentation/concatenation
constexpr size_t SIF_BITS = 1;          // Segment indicator flag
constexpr size_t CIF_BITS = 1;          // Concatenation indicator flag
constexpr size_t LENGTH_EXT_BITS = 13;  // Extended length field

// Segmentation header (when SIF=1)
constexpr size_t SEGMENT_SEQ_BITS = 5;   // Sequence number (0-31)
constexpr size_t LAST_SEGMENT_BITS = 1;  // Last segment indicator
constexpr size_t SID_BITS = 8;           // Sub-stream identifier

// Minimum header sizes
constexpr size_t MIN_HEADER_BYTES = 2;         // Single-header mode
constexpr size_t ADDITIONAL_HEADER_BYTES = 3;  // Additional header mode
constexpr size_t SEGMENT_HEADER_BYTES = 2;     // Segmentation header

// MPEG-2 TS packet size
constexpr size_t TS_PACKET_SIZE = 188;

// Signaling table types
constexpr uint8_t TABLE_LLS = 0x01;  // Low-level signaling
constexpr uint8_t TABLE_SLT = 0x02;  // Service list table
constexpr uint8_t TABLE_RRT = 0x03;  // Region rating table
constexpr uint8_t TABLE_CAP = 0x04;  // Common alerting protocol
}  // namespace alp_fields

}  // namespace framing
}  // namespace atsc3
