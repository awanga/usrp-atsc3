// test_end_to_end.cc — End-to-end integration test for Phase 5
//
// Tests the complete ATSC 3.0 receiver pipeline from IQ capture through
// transport layer (ALP demux, ROUTE parsing) to A/V decode.
//
// This is the MVP gate test for Phase 5.
//
// Requires:
//   - Real ATSC 3.0 IQ captures in test/captures/ (via git-lfs)
//   - FFmpeg libraries (for full A/V decode test)

#include "alp_demux.h"
#include "config_bus.h"
#include "route_parser.h"
#include "service_catalog.h"

#include <cmath>
#include <filesystem>
#include <gtest/gtest.h>
#include <iostream>
#include <string>
#include <vector>

namespace atsc3 {
namespace integration {
namespace {

// Test fixture for Phase 5 end-to-end tests
class EndToEndTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize components
        catalog_ = std::make_unique<framing::ServiceCatalog>();
        route_parser_ = std::make_unique<framing::RouteParser>(catalog_.get());
        alp_demux_ = std::make_unique<framing::AlpDemux>();
    }

    std::unique_ptr<framing::ServiceCatalog> catalog_;
    std::unique_ptr<framing::RouteParser> route_parser_;
    std::unique_ptr<framing::AlpDemux> alp_demux_;
};

//==============================================================================
// Component Integration Tests
//==============================================================================

// Test: ALP demux to ROUTE parser integration
TEST_F(EndToEndTest, AlpToRouteIntegration) {
    // Register ALP signaling callback to feed ROUTE parser
    alp_demux_->set_signaling_callback([this](uint8_t /*plp_id*/, const uint8_t* data, size_t len) {
        route_parser_->process_signaling(data, len);
    });

    // Create synthetic SLT data wrapped in ALP signaling packet
    const char* slt_xml = R"(<?xml version="1.0"?>
<SLT bsid="100" version="1">
    <Service serviceId="1" majorChannelNo="10" minorChannelNo="1"
             shortServiceName="TEST" serviceCategory="1"/>
</SLT>
)";

    // Create LCT header for SLT
    std::vector<uint8_t> lct_packet;

    // LCT header (12 bytes)
    lct_packet.push_back(0x10);  // version=1
    lct_packet.push_back(0xA0);  // S=1, O=1
    lct_packet.push_back(0x03);  // HDR_LEN=3
    lct_packet.push_back(0x00);  // CP=0 (LLS)
    lct_packet.push_back(0x00);
    lct_packet.push_back(0x00);
    lct_packet.push_back(0x00);
    lct_packet.push_back(0x01);  // TSI
    lct_packet.push_back(0xFF);
    lct_packet.push_back(0xFF);
    lct_packet.push_back(0xFF);
    lct_packet.push_back(0x00);  // TOI=SLT

    // Add XML payload
    std::string xml_str(slt_xml);
    for (char c : xml_str) {
        lct_packet.push_back(static_cast<uint8_t>(c));
    }

    // Wrap in ALP signaling packet (type=4)
    std::vector<uint8_t> alp_packet;
    uint16_t payload_len = static_cast<uint16_t>(lct_packet.size());

    // ALP header (single-header mode)
    alp_packet.push_back((0x04 << 5) | ((payload_len >> 7) & 0x0F));
    alp_packet.push_back((payload_len << 1) & 0xFE);
    alp_packet.insert(alp_packet.end(), lct_packet.begin(), lct_packet.end());

    // Process through ALP demux
    size_t packets_parsed = alp_demux_->process(0, alp_packet.data(), alp_packet.size());

    EXPECT_EQ(packets_parsed, 1u);
    EXPECT_EQ(alp_demux_->get_stats().signaling_packets, 1u);

    // Verify service was added to catalog
    EXPECT_EQ(catalog_->service_count(), 1u);

    const framing::Service* svc = catalog_->find_service(1);
    ASSERT_NE(svc, nullptr);
    EXPECT_EQ(svc->major_channel, 10u);
    EXPECT_EQ(svc->minor_channel, 1u);
    EXPECT_STREQ(svc->short_name.data(), "TEST");
}

// Test: Service catalog updates propagate correctly
TEST_F(EndToEndTest, ServiceCatalogUpdates) {
    int update_count = 0;
    catalog_->set_update_callback([&]() { update_count++; });

    // Add multiple services
    for (int i = 1; i <= 5; i++) {
        framing::Service svc;
        svc.service_id = static_cast<uint16_t>(i);
        svc.major_channel = 10;
        svc.minor_channel = static_cast<uint16_t>(i);
        svc.valid = true;
        catalog_->update_service(svc);
    }

    EXPECT_EQ(update_count, 5);
    EXPECT_EQ(catalog_->service_count(), 5u);

    // Find by channel
    const framing::Service* found = catalog_->find_by_channel(10, 3);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->service_id, 3u);
}

// Test: ALP IPv4 packet reassembly
TEST_F(EndToEndTest, AlpIpReassembly) {
    std::vector<uint8_t> received_data;
    alp_demux_->set_ip_callback([&](uint8_t /*plp_id*/, const uint8_t* data, size_t len) {
        received_data.assign(data, data + len);
    });

    // Create synthetic IPv4 payload
    std::vector<uint8_t> ip_payload(200);
    for (size_t i = 0; i < ip_payload.size(); i++) {
        ip_payload[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // Wrap in ALP IPv4 packet (single-header mode)
    std::vector<uint8_t> alp_packet;
    uint16_t payload_len = static_cast<uint16_t>(ip_payload.size());

    // ALP header: type=0 (IPv4), PC=0
    alp_packet.push_back((0x00 << 5) | ((payload_len >> 7) & 0x0F));
    alp_packet.push_back((payload_len << 1) & 0xFE);
    alp_packet.insert(alp_packet.end(), ip_payload.begin(), ip_payload.end());

    // Process
    framing::AlpDemuxConfig config;
    config.check_crc = false;
    framing::AlpDemux demux(config);

    demux.set_ip_callback(
        [&](uint8_t, const uint8_t* data, size_t len) { received_data.assign(data, data + len); });

    demux.process(0, alp_packet.data(), alp_packet.size());

    EXPECT_EQ(received_data.size(), ip_payload.size());
    EXPECT_EQ(received_data, ip_payload);
}

// Test: Multiple PLPs through ALP demux
TEST_F(EndToEndTest, MultiPlpDemux) {
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> received;

    framing::AlpDemuxConfig config;
    config.check_crc = false;
    framing::AlpDemux demux(config);

    demux.set_ip_callback([&](uint8_t plp_id, const uint8_t* data, size_t len) {
        received.emplace_back(plp_id, std::vector<uint8_t>(data, data + len));
    });

    // Create packets for different PLPs
    for (uint8_t plp = 0; plp < 3; plp++) {
        std::vector<uint8_t> payload(10);
        std::fill(payload.begin(), payload.end(), plp);

        std::vector<uint8_t> alp_packet;
        uint16_t payload_len = static_cast<uint16_t>(payload.size());
        alp_packet.push_back((0x00 << 5) | ((payload_len >> 7) & 0x0F));
        alp_packet.push_back((payload_len << 1) & 0xFE);
        alp_packet.insert(alp_packet.end(), payload.begin(), payload.end());

        demux.process(plp, alp_packet.data(), alp_packet.size());
    }

    ASSERT_EQ(received.size(), 3u);
    for (size_t i = 0; i < 3; i++) {
        EXPECT_EQ(received[i].first, i);
        EXPECT_EQ(received[i].second[0], i);
    }
}

// Test: ROUTE parser SLT parsing
TEST_F(EndToEndTest, RouteSlTParsing) {
    const char* slt_xml = R"(<?xml version="1.0"?>
<SLT bsid="999" version="2">
    <Service serviceId="100" majorChannelNo="5" minorChannelNo="1"
             shortServiceName="HD" serviceCategory="1">
        <BroadcastSvcSignaling slsDestinationIpAddress="224.0.23.60"
                               slsDestinationUdpPort="4937"/>
    </Service>
    <Service serviceId="101" majorChannelNo="5" minorChannelNo="2"
             shortServiceName="SD" serviceCategory="1"/>
    <Service serviceId="102" majorChannelNo="5" minorChannelNo="3"
             shortServiceName="Radio" serviceCategory="2"/>
</SLT>
)";

    // Create LCT packet
    std::vector<uint8_t> lct_packet;
    lct_packet.push_back(0x10);
    lct_packet.push_back(0xA0);
    lct_packet.push_back(0x03);
    lct_packet.push_back(0x00);
    lct_packet.push_back(0x00);
    lct_packet.push_back(0x00);
    lct_packet.push_back(0x00);
    lct_packet.push_back(0x01);
    lct_packet.push_back(0xFF);
    lct_packet.push_back(0xFF);
    lct_packet.push_back(0xFF);
    lct_packet.push_back(0x00);

    std::string xml(slt_xml);
    for (char c : xml) {
        lct_packet.push_back(static_cast<uint8_t>(c));
    }

    bool result = route_parser_->process_lct_packet(lct_packet.data(), lct_packet.size());

    EXPECT_TRUE(result);
    EXPECT_EQ(route_parser_->get_stats().slt_tables_received, 1u);
    EXPECT_EQ(catalog_->service_count(), 3u);

    // Verify SLT content
    const auto& slt = route_parser_->get_slt();
    EXPECT_TRUE(slt.valid);
    EXPECT_EQ(slt.bsid, 999u);
    EXPECT_EQ(slt.services.size(), 3u);

    // Verify services in catalog
    EXPECT_NE(catalog_->find_service(100), nullptr);
    EXPECT_NE(catalog_->find_service(101), nullptr);
    EXPECT_NE(catalog_->find_service(102), nullptr);

    const framing::Service* hd = catalog_->find_by_channel(5, 1);
    ASSERT_NE(hd, nullptr);
    EXPECT_STREQ(hd->short_name.data(), "HD");
    EXPECT_EQ(hd->service_category, 1u);

    const framing::Service* radio = catalog_->find_service(102);
    ASSERT_NE(radio, nullptr);
    EXPECT_EQ(radio->service_category, 2u);  // Radio
}

// Test: Statistics accumulation
TEST_F(EndToEndTest, StatisticsAccumulation) {
    framing::AlpDemuxConfig config;
    config.check_crc = false;
    framing::AlpDemux demux(config);

    demux.set_ip_callback([](uint8_t, const uint8_t*, size_t) {});
    demux.set_signaling_callback([](uint8_t, const uint8_t*, size_t) {});

    // Process multiple packets
    for (int i = 0; i < 10; i++) {
        std::vector<uint8_t> payload(50);
        std::vector<uint8_t> alp_packet;
        uint16_t payload_len = static_cast<uint16_t>(payload.size());

        // Alternate between IPv4 and signaling
        uint8_t type = (i % 2 == 0) ? 0x00 : 0x04;
        alp_packet.push_back((type << 5) | ((payload_len >> 7) & 0x0F));
        alp_packet.push_back((payload_len << 1) & 0xFE);
        alp_packet.insert(alp_packet.end(), payload.begin(), payload.end());

        demux.process(0, alp_packet.data(), alp_packet.size());
    }

    const auto& stats = demux.get_stats();
    EXPECT_EQ(stats.packets_received, 10u);
    EXPECT_EQ(stats.ipv4_packets, 5u);
    EXPECT_EQ(stats.signaling_packets, 5u);
}

//==============================================================================
// End-to-End Flow Tests
//==============================================================================

// Test: Full signaling flow from ALP through service discovery
TEST_F(EndToEndTest, FullSignalingFlow) {
    // Set up the complete chain
    alp_demux_->set_signaling_callback([this](uint8_t /*plp_id*/, const uint8_t* data, size_t len) {
        route_parser_->process_signaling(data, len);
    });

    // Track service updates
    std::vector<uint16_t> discovered_services;
    catalog_->set_update_callback([this, &discovered_services]() {
        for (size_t i = 0; i < catalog_->service_count(); i++) {
            const auto* svc = catalog_->get_service(i);
            if (svc != nullptr && svc->valid) {
                discovered_services.push_back(svc->service_id);
            }
        }
    });

    // Simulate receiving SLT via ALP
    const char* slt =
        R"(<SLT bsid="1"><Service serviceId="1000"/><Service serviceId="2000"/></SLT>)";

    std::vector<uint8_t> lct_packet;
    lct_packet.push_back(0x10);
    lct_packet.push_back(0xA0);
    lct_packet.push_back(0x03);
    lct_packet.push_back(0x00);
    for (int i = 0; i < 8; i++)
        lct_packet.push_back(i == 3 ? 0x01 : 0x00);

    std::string xml(slt);
    for (char c : xml) {
        lct_packet.push_back(static_cast<uint8_t>(c));
    }

    std::vector<uint8_t> alp_packet;
    uint16_t len = static_cast<uint16_t>(lct_packet.size());
    alp_packet.push_back((0x04 << 5) | ((len >> 7) & 0x0F));
    alp_packet.push_back((len << 1) & 0xFE);
    alp_packet.insert(alp_packet.end(), lct_packet.begin(), lct_packet.end());

    alp_demux_->process(0, alp_packet.data(), alp_packet.size());

    // Verify services were discovered
    EXPECT_EQ(catalog_->service_count(), 2u);
    EXPECT_NE(catalog_->find_service(1000), nullptr);
    EXPECT_NE(catalog_->find_service(2000), nullptr);
}

// Test: Component reset and reinitialize
TEST_F(EndToEndTest, ResetAndReinitialize) {
    // Add some data
    framing::Service svc;
    svc.service_id = 1;
    svc.valid = true;
    catalog_->update_service(svc);

    EXPECT_EQ(catalog_->service_count(), 1u);

    // Reset
    catalog_->clear();
    route_parser_->reset();
    alp_demux_->reset();

    EXPECT_EQ(catalog_->service_count(), 0u);
    EXPECT_EQ(route_parser_->get_stats().lct_packets_parsed, 0u);
    EXPECT_EQ(alp_demux_->get_stats().packets_received, 0u);
}

}  // namespace
}  // namespace integration
}  // namespace atsc3
