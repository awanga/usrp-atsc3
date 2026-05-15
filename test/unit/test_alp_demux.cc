// test_alp_demux.cc — Unit tests for lib/framing/alp_demux.h
//
// Tests ALP packet parsing and IP datagram reassembly

#include "alp_demux.h"

#include <algorithm>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace atsc3 {
namespace framing {
namespace {

//==============================================================================
// Test Helpers
//==============================================================================

// Create a simple ALP packet with IPv4 payload (single-header mode)
std::vector<uint8_t> create_ipv4_packet(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet;

    // Single-header mode: PC=0
    // Byte 0: packet_type[7:5]=000 (IPv4) + PC[4]=0 + length[3:0]
    // Byte 1: length[10:4]
    uint16_t length = static_cast<uint16_t>(payload.size());

    uint8_t byte0 = (0x00 << 5) |            // packet_type = IPv4
                    (0 << 4) |               // PC = 0 (single header)
                    ((length >> 7) & 0x0F);  // length MSB
    uint8_t byte1 = (length << 1) & 0xFE;    // length LSB (shifted)

    packet.push_back(byte0);
    packet.push_back(byte1);
    packet.insert(packet.end(), payload.begin(), payload.end());

    return packet;
}

// Create ALP signaling packet
std::vector<uint8_t> create_signaling_packet(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet;

    uint16_t length = static_cast<uint16_t>(payload.size());

    uint8_t byte0 = (0x04 << 5) |  // packet_type = SIGNALING
                    (0 << 4) |     // PC = 0
                    ((length >> 7) & 0x0F);
    uint8_t byte1 = (length << 1) & 0xFE;

    packet.push_back(byte0);
    packet.push_back(byte1);
    packet.insert(packet.end(), payload.begin(), payload.end());

    return packet;
}

// Create ALP MPEG-2 TS packet
std::vector<uint8_t> create_ts_packet(const std::vector<uint8_t>& ts_data) {
    std::vector<uint8_t> packet;

    uint16_t length = static_cast<uint16_t>(ts_data.size());

    uint8_t byte0 = (0x07 << 5) |  // packet_type = MPEG2_TS
                    (0 << 4) |     // PC = 0
                    ((length >> 7) & 0x0F);
    uint8_t byte1 = (length << 1) & 0xFE;

    packet.push_back(byte0);
    packet.push_back(byte1);
    packet.insert(packet.end(), ts_data.begin(), ts_data.end());

    return packet;
}

// Create fragmented ALP packets (additional header mode with segmentation)
std::vector<std::vector<uint8_t>> create_fragmented_packets(const std::vector<uint8_t>& payload,
                                                            size_t fragment_size,
                                                            uint8_t sub_stream_id) {
    std::vector<std::vector<uint8_t>> fragments;
    size_t offset = 0;
    uint8_t sequence = 0;

    while (offset < payload.size()) {
        std::vector<uint8_t> packet;
        size_t remaining = payload.size() - offset;
        size_t this_size = std::min(remaining, fragment_size);
        bool is_last = (offset + this_size >= payload.size());

        // Additional header mode with segmentation (PC=1, HM=1, SIF=1)
        // Byte 0: packet_type[7:5]=000 + PC[4]=1 + HM[3]=1 + (unused)[2:0]
        // Byte 1: SIF[7]=1 + CIF[6]=0 + length[12:7]
        // Byte 2: length[6:0] << 1
        // Segmentation header (when SIF=1):
        // Byte 3: seq[7:3] + LSI[2] + SID[1:0]
        // Byte 4: SID[7:2] + (unused)[1:0]

        uint16_t length = static_cast<uint16_t>(this_size);

        uint8_t byte0 = (0x00 << 5) |            // packet_type = IPv4
                        (1 << 4) |               // PC = 1 (additional header)
                        (1 << 3);                // HM = 1 (full additional header)
        uint8_t byte1 = (1 << 7) |               // SIF = 1 (segmentation)
                        (0 << 6) |               // CIF = 0
                        ((length >> 7) & 0x3F);  // length[12:7]
        uint8_t byte2 = ((length & 0x7F) << 1);  // length[6:0] << 1

        // Segmentation header
        uint8_t seg_byte0 =
            ((sequence & 0x1F) << 3) | ((is_last ? 1 : 0) << 2) | ((sub_stream_id >> 6) & 0x03);
        uint8_t seg_byte1 = ((sub_stream_id & 0x3F) << 2);

        packet.push_back(byte0);
        packet.push_back(byte1);
        packet.push_back(byte2);
        packet.push_back(seg_byte0);
        packet.push_back(seg_byte1);

        // Add payload fragment
        packet.insert(packet.end(), payload.begin() + offset, payload.begin() + offset + this_size);

        fragments.push_back(packet);
        offset += this_size;
        sequence++;
    }

    return fragments;
}

//==============================================================================
// Construction Tests
//==============================================================================

TEST(AlpDemuxTest, DefaultConstruction) {
    AlpDemux demux;

    EXPECT_TRUE(demux.get_config().check_crc);
    EXPECT_EQ(demux.get_config().max_datagram_size, 65535u);
    EXPECT_EQ(demux.get_stats().packets_received, 0u);
}

TEST(AlpDemuxTest, CustomConfig) {
    AlpDemuxConfig config;
    config.check_crc = false;
    config.max_datagram_size = 4096;

    AlpDemux demux(config);

    EXPECT_FALSE(demux.get_config().check_crc);
    EXPECT_EQ(demux.get_config().max_datagram_size, 4096u);
}

//==============================================================================
// IPv4 Packet Tests
//==============================================================================

TEST(AlpDemuxTest, ProcessSingleIpv4Packet) {
    AlpDemuxConfig config;
    config.check_crc = false;  // Disable CRC for simple tests

    AlpDemux demux(config);

    std::vector<uint8_t> received_data;
    uint8_t received_plp = 255;

    demux.set_ip_callback([&](uint8_t plp, const uint8_t* data, size_t len) {
        received_plp = plp;
        received_data.assign(data, data + len);
    });

    // Create test payload
    std::vector<uint8_t> payload = {0x45, 0x00, 0x00, 0x14,  // IP header start
                                    0x12, 0x34, 0x00, 0x00, 0x40, 0x06,
                                    0x00, 0x00, 0xC0, 0xA8, 0x01, 0x01,  // src IP
                                    0xC0, 0xA8, 0x01, 0x02};             // dst IP

    auto packet = create_ipv4_packet(payload);
    size_t parsed = demux.process(1, packet.data(), packet.size());

    EXPECT_EQ(parsed, 1u);
    EXPECT_EQ(received_plp, 1u);
    EXPECT_EQ(received_data, payload);
    EXPECT_EQ(demux.get_stats().ipv4_packets, 1u);
    EXPECT_EQ(demux.get_stats().datagrams_reassembled, 1u);
}

TEST(AlpDemuxTest, ProcessMultipleIpv4Packets) {
    AlpDemuxConfig config;
    config.check_crc = false;

    AlpDemux demux(config);

    int callback_count = 0;
    demux.set_ip_callback([&](uint8_t, const uint8_t*, size_t) { callback_count++; });

    // Create multiple packets
    std::vector<uint8_t> payload1 = {0x01, 0x02, 0x03, 0x04};
    std::vector<uint8_t> payload2 = {0x05, 0x06, 0x07, 0x08};
    std::vector<uint8_t> payload3 = {0x09, 0x0A, 0x0B, 0x0C};

    auto pkt1 = create_ipv4_packet(payload1);
    auto pkt2 = create_ipv4_packet(payload2);
    auto pkt3 = create_ipv4_packet(payload3);

    // Concatenate packets
    std::vector<uint8_t> stream;
    stream.insert(stream.end(), pkt1.begin(), pkt1.end());
    stream.insert(stream.end(), pkt2.begin(), pkt2.end());
    stream.insert(stream.end(), pkt3.begin(), pkt3.end());

    size_t parsed = demux.process(0, stream.data(), stream.size());

    EXPECT_EQ(parsed, 3u);
    EXPECT_EQ(callback_count, 3);
    EXPECT_EQ(demux.get_stats().packets_received, 3u);
}

TEST(AlpDemuxTest, ProcessPartialPacket) {
    AlpDemuxConfig config;
    config.check_crc = false;

    AlpDemux demux(config);

    int callback_count = 0;
    demux.set_ip_callback([&](uint8_t, const uint8_t*, size_t) { callback_count++; });

    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    auto packet = create_ipv4_packet(payload);

    // Send first half
    size_t half = packet.size() / 2;
    demux.process(0, packet.data(), half);
    EXPECT_EQ(callback_count, 0);  // Not yet complete

    // Send second half
    demux.process(0, packet.data() + half, packet.size() - half);
    EXPECT_EQ(callback_count, 1);  // Now complete
}

//==============================================================================
// Signaling Packet Tests
//==============================================================================

TEST(AlpDemuxTest, ProcessSignalingPacket) {
    AlpDemuxConfig config;
    config.check_crc = false;

    AlpDemux demux(config);

    std::vector<uint8_t> received_data;
    demux.set_signaling_callback(
        [&](uint8_t, const uint8_t* data, size_t len) { received_data.assign(data, data + len); });

    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04};  // Signaling data
    auto packet = create_signaling_packet(payload);

    demux.process(0, packet.data(), packet.size());

    EXPECT_EQ(received_data, payload);
    EXPECT_EQ(demux.get_stats().signaling_packets, 1u);
}

//==============================================================================
// MPEG-2 TS Packet Tests
//==============================================================================

TEST(AlpDemuxTest, ProcessTsPacket) {
    AlpDemuxConfig config;
    config.check_crc = false;

    AlpDemux demux(config);

    std::vector<uint8_t> received_data;
    demux.set_ts_callback(
        [&](uint8_t, const uint8_t* data, size_t len) { received_data.assign(data, data + len); });

    // Create 188-byte TS packet
    std::vector<uint8_t> ts_payload(188, 0x47);  // Start with sync byte
    ts_payload[0] = 0x47;                        // TS sync byte

    auto packet = create_ts_packet(ts_payload);

    demux.process(0, packet.data(), packet.size());

    EXPECT_EQ(received_data.size(), 188u);
    EXPECT_EQ(received_data[0], 0x47);
    EXPECT_EQ(demux.get_stats().ts_packets, 1u);
}

//==============================================================================
// Fragmentation/Reassembly Tests
//==============================================================================

TEST(AlpDemuxTest, ReassembleFragmentedPacket) {
    AlpDemuxConfig config;
    config.check_crc = false;

    AlpDemux demux(config);

    std::vector<uint8_t> received_data;
    demux.set_ip_callback(
        [&](uint8_t, const uint8_t* data, size_t len) { received_data.assign(data, data + len); });

    // Create large payload that will be fragmented
    std::vector<uint8_t> payload(500);
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // Fragment into 100-byte chunks
    auto fragments = create_fragmented_packets(payload, 100, 1);

    EXPECT_EQ(fragments.size(), 5u);

    // Process all fragments
    for (const auto& frag : fragments) {
        demux.process(0, frag.data(), frag.size());
    }

    EXPECT_EQ(received_data, payload);
    EXPECT_EQ(demux.get_stats().fragments_received, 5u);
    EXPECT_EQ(demux.get_stats().datagrams_reassembled, 1u);
}

TEST(AlpDemuxTest, ReassemblyOutOfOrder) {
    AlpDemuxConfig config;
    config.check_crc = false;

    AlpDemux demux(config);

    int callback_count = 0;
    demux.set_ip_callback([&](uint8_t, const uint8_t*, size_t) { callback_count++; });

    std::vector<uint8_t> payload(200);
    auto fragments = create_fragmented_packets(payload, 100, 1);

    ASSERT_EQ(fragments.size(), 2u);

    // Send second fragment first (out of order)
    demux.process(0, fragments[1].data(), fragments[1].size());

    // Should fail to reassemble
    EXPECT_EQ(callback_count, 0);
    EXPECT_GT(demux.get_stats().reassembly_errors, 0u);
}

TEST(AlpDemuxTest, MultipleStreamsReassembly) {
    AlpDemuxConfig config;
    config.check_crc = false;

    AlpDemux demux(config);

    std::vector<std::vector<uint8_t>> received;
    demux.set_ip_callback(
        [&](uint8_t, const uint8_t* data, size_t len) { received.emplace_back(data, data + len); });

    // Create two different payloads
    std::vector<uint8_t> payload1(150);
    std::vector<uint8_t> payload2(150);
    std::fill(payload1.begin(), payload1.end(), 0xAA);
    std::fill(payload2.begin(), payload2.end(), 0xBB);

    // Fragment both with different sub-stream IDs
    auto frags1 = create_fragmented_packets(payload1, 100, 1);
    auto frags2 = create_fragmented_packets(payload2, 100, 2);

    // Interleave fragments
    demux.process(0, frags1[0].data(), frags1[0].size());
    demux.process(0, frags2[0].data(), frags2[0].size());
    demux.process(0, frags1[1].data(), frags1[1].size());
    demux.process(0, frags2[1].data(), frags2[1].size());

    EXPECT_EQ(received.size(), 2u);
}

//==============================================================================
// Error Handling Tests
//==============================================================================

TEST(AlpDemuxTest, HandleNullInput) {
    AlpDemux demux;

    size_t result = demux.process(0, nullptr, 100);

    EXPECT_EQ(result, 0u);
}

TEST(AlpDemuxTest, HandleZeroLength) {
    AlpDemux demux;

    std::vector<uint8_t> data = {0x00};
    size_t result = demux.process(0, data.data(), 0);

    EXPECT_EQ(result, 0u);
}

TEST(AlpDemuxTest, HandleInvalidHeader) {
    AlpDemuxConfig config;
    config.check_crc = false;

    AlpDemux demux(config);

    // Invalid packet type (reserved)
    std::vector<uint8_t> invalid = {0x20, 0x04, 0x01, 0x02};  // type = 1 (reserved)
    demux.process(0, invalid.data(), invalid.size());

    EXPECT_GT(demux.get_stats().header_errors, 0u);
}

TEST(AlpDemuxTest, HandleTruncatedHeader) {
    AlpDemux demux;

    // Only 1 byte - not enough for header
    std::vector<uint8_t> data = {0x00};
    size_t result = demux.process(0, data.data(), data.size());

    EXPECT_EQ(result, 0u);
}

//==============================================================================
// Reset Tests
//==============================================================================

TEST(AlpDemuxTest, Reset) {
    AlpDemuxConfig config;
    config.check_crc = false;

    AlpDemux demux(config);

    // Process some packets
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
    auto packet = create_ipv4_packet(payload);
    demux.process(0, packet.data(), packet.size());

    EXPECT_GT(demux.get_stats().packets_received, 0u);

    // Reset
    demux.reset();

    EXPECT_EQ(demux.get_stats().packets_received, 0u);
}

TEST(AlpDemuxTest, ResetStats) {
    AlpDemuxConfig config;
    config.check_crc = false;

    AlpDemux demux(config);

    std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
    auto packet = create_ipv4_packet(payload);
    demux.process(0, packet.data(), packet.size());

    demux.reset_stats();

    EXPECT_EQ(demux.get_stats().packets_received, 0u);
    EXPECT_EQ(demux.get_stats().ipv4_packets, 0u);
}

//==============================================================================
// Callback Tests
//==============================================================================

TEST(AlpDemuxTest, NoCallbackSet) {
    AlpDemuxConfig config;
    config.check_crc = false;

    AlpDemux demux(config);

    // No callback set - should not crash
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
    auto packet = create_ipv4_packet(payload);

    EXPECT_NO_THROW(demux.process(0, packet.data(), packet.size()));
    EXPECT_EQ(demux.get_stats().ipv4_packets, 1u);
}

TEST(AlpDemuxTest, PlpIdPassthrough) {
    AlpDemuxConfig config;
    config.check_crc = false;

    AlpDemux demux(config);

    std::vector<uint8_t> received_plps;
    demux.set_ip_callback(
        [&](uint8_t plp, const uint8_t*, size_t) { received_plps.push_back(plp); });

    std::vector<uint8_t> payload = {0x01, 0x02};
    auto packet = create_ipv4_packet(payload);

    demux.process(5, packet.data(), packet.size());
    demux.process(10, packet.data(), packet.size());
    demux.process(63, packet.data(), packet.size());

    ASSERT_EQ(received_plps.size(), 3u);
    EXPECT_EQ(received_plps[0], 5u);
    EXPECT_EQ(received_plps[1], 10u);
    EXPECT_EQ(received_plps[2], 63u);
}

//==============================================================================
// Large Packet Tests
//==============================================================================

TEST(AlpDemuxTest, LargePayload) {
    AlpDemuxConfig config;
    config.check_crc = false;

    AlpDemux demux(config);

    std::vector<uint8_t> received_data;
    demux.set_ip_callback(
        [&](uint8_t, const uint8_t* data, size_t len) { received_data.assign(data, data + len); });

    // Create large payload (1500 bytes - typical MTU)
    std::vector<uint8_t> payload(1500);
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto packet = create_ipv4_packet(payload);
    demux.process(0, packet.data(), packet.size());

    EXPECT_EQ(received_data, payload);
}

//==============================================================================
// Statistics Tests
//==============================================================================

TEST(AlpDemuxTest, StatisticsAccuracy) {
    AlpDemuxConfig config;
    config.check_crc = false;

    AlpDemux demux(config);

    // Set up dummy callbacks
    demux.set_ip_callback([](uint8_t, const uint8_t*, size_t) {});
    demux.set_signaling_callback([](uint8_t, const uint8_t*, size_t) {});

    // Create various packet types
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04};

    auto ip_pkt = create_ipv4_packet(payload);
    auto sig_pkt = create_signaling_packet(payload);

    // Process packets
    demux.process(0, ip_pkt.data(), ip_pkt.size());
    demux.process(0, ip_pkt.data(), ip_pkt.size());
    demux.process(0, sig_pkt.data(), sig_pkt.size());

    const auto& stats = demux.get_stats();
    EXPECT_EQ(stats.packets_received, 3u);
    EXPECT_EQ(stats.ipv4_packets, 2u);
    EXPECT_EQ(stats.signaling_packets, 1u);
    EXPECT_EQ(stats.datagrams_reassembled, 2u);
}

//==============================================================================
// Flush Tests
//==============================================================================

TEST(AlpDemuxTest, FlushIncomplete) {
    AlpDemuxConfig config;
    config.check_crc = false;

    AlpDemux demux(config);

    int callback_count = 0;
    demux.set_ip_callback([&](uint8_t, const uint8_t*, size_t) { callback_count++; });

    // Send first fragment only
    std::vector<uint8_t> payload(200);
    auto fragments = create_fragmented_packets(payload, 100, 1);
    demux.process(0, fragments[0].data(), fragments[0].size());

    // Flush should not deliver incomplete reassembly
    demux.flush();

    EXPECT_EQ(callback_count, 0);
    EXPECT_GT(demux.get_stats().reassembly_errors, 0u);
}

}  // namespace
}  // namespace framing
}  // namespace atsc3
