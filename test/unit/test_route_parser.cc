// test_route_parser.cc — Unit tests for lib/framing/route_parser.h
//
// Tests ROUTE signaling parsing and service discovery

#include "route_parser.h"
#include "service_catalog.h"

#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace atsc3 {
namespace framing {
namespace {

//==============================================================================
// Test Data - Sample SLT XML
//==============================================================================

const char* SAMPLE_SLT_XML = R"(<?xml version="1.0" encoding="UTF-8"?>
<SLT bsid="1234" version="1">
    <Service serviceId="1001" majorChannelNo="5" minorChannelNo="1"
             shortServiceName="WXYZ" serviceCategory="1">
        <BroadcastSvcSignaling slsDestinationIpAddress="224.0.23.60"
                               slsDestinationUdpPort="4937"
                               slsSourceIpAddress="10.0.0.1"/>
    </Service>
    <Service serviceId="1002" majorChannelNo="5" minorChannelNo="2"
             shortServiceName="WXYZ-2" serviceCategory="1">
        <BroadcastSvcSignaling slsDestinationIpAddress="224.0.23.61"
                               slsDestinationUdpPort="4938"
                               slsSourceIpAddress="10.0.0.2"/>
    </Service>
    <Service serviceId="1003" majorChannelNo="5" minorChannelNo="3"
             shortServiceName="Radio1" serviceCategory="2">
    </Service>
</SLT>
)";

const char* SAMPLE_MPD_XML = R"(<?xml version="1.0" encoding="UTF-8"?>
<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="dynamic">
    <BaseURL>http://example.com/stream/</BaseURL>
    <Period id="1">
        <AdaptationSet mimeType="video/mp4" codecs="hev1.1.6.L93.B0">
            <SegmentTemplate timescale="90000" media="video_$Number$.m4s"/>
        </AdaptationSet>
    </Period>
</MPD>
)";

// Create LCT packet with SLT payload
std::vector<uint8_t> create_lct_slt_packet(const char* xml) {
    std::vector<uint8_t> packet;

    size_t xml_len = std::strlen(xml);

    // LCT header (simplified - 12 bytes minimum for TSI+TOI)
    // Byte 0: version=1, C=0, PSI=0 -> 0x10
    // Byte 1: S=1, O=1, H=0, reserved=0, A=0, B=0 -> 0xA0
    // Byte 2: HDR_LEN = 3 (12 bytes / 4)
    // Byte 3: CP = 0 (LLS)
    packet.push_back(0x10);  // version=1
    packet.push_back(0xA0);  // S=1, O=1 (32-bit TSI and TOI)
    packet.push_back(0x03);  // HDR_LEN = 3
    packet.push_back(route_codepoint::LLS);

    // TSI (4 bytes) - use 0x1001 for service
    packet.push_back(0x00);
    packet.push_back(0x00);
    packet.push_back(0x10);
    packet.push_back(0x01);

    // TOI (4 bytes) - SLT marker
    packet.push_back(0xFF);
    packet.push_back(0xFF);
    packet.push_back(0xFF);
    packet.push_back(0x00);

    // XML payload
    for (size_t i = 0; i < xml_len; i++) {
        packet.push_back(static_cast<uint8_t>(xml[i]));
    }

    return packet;
}

// Create LCT packet with MPD payload
std::vector<uint8_t> create_lct_mpd_packet(const char* xml, uint16_t service_id) {
    std::vector<uint8_t> packet;

    size_t xml_len = std::strlen(xml);

    packet.push_back(0x10);  // version=1
    packet.push_back(0xA0);  // S=1, O=1
    packet.push_back(0x03);  // HDR_LEN = 3
    packet.push_back(route_codepoint::SLS);

    // TSI (service_id)
    packet.push_back(0x00);
    packet.push_back(0x00);
    packet.push_back(static_cast<uint8_t>(service_id >> 8));
    packet.push_back(static_cast<uint8_t>(service_id & 0xFF));

    // TOI = MPD marker
    packet.push_back(0xFF);
    packet.push_back(0xFF);
    packet.push_back(0xFF);
    packet.push_back(0x02);

    // XML payload
    for (size_t i = 0; i < xml_len; i++) {
        packet.push_back(static_cast<uint8_t>(xml[i]));
    }

    return packet;
}

//==============================================================================
// ServiceCatalog Tests
//==============================================================================

TEST(ServiceCatalogTest, DefaultConstruction) {
    ServiceCatalog catalog;

    EXPECT_EQ(catalog.service_count(), 0u);
}

TEST(ServiceCatalogTest, AddService) {
    ServiceCatalog catalog;

    Service svc;
    svc.service_id = 1001;
    svc.major_channel = 5;
    svc.minor_channel = 1;
    svc.valid = true;
    std::strcpy(svc.short_name.data(), "TEST");

    bool result = catalog.update_service(svc);

    EXPECT_TRUE(result);
    EXPECT_EQ(catalog.service_count(), 1u);
}

TEST(ServiceCatalogTest, FindServiceById) {
    ServiceCatalog catalog;

    Service svc;
    svc.service_id = 1001;
    svc.major_channel = 5;
    svc.minor_channel = 1;
    svc.valid = true;

    catalog.update_service(svc);

    const Service* found = catalog.find_service(1001);

    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->service_id, 1001u);
    EXPECT_EQ(found->major_channel, 5u);
}

TEST(ServiceCatalogTest, FindServiceByChannel) {
    ServiceCatalog catalog;

    Service svc;
    svc.service_id = 1001;
    svc.major_channel = 5;
    svc.minor_channel = 3;
    svc.valid = true;

    catalog.update_service(svc);

    const Service* found = catalog.find_by_channel(5, 3);

    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->service_id, 1001u);
}

TEST(ServiceCatalogTest, UpdateExistingService) {
    ServiceCatalog catalog;

    Service svc1;
    svc1.service_id = 1001;
    svc1.major_channel = 5;
    svc1.minor_channel = 1;
    svc1.valid = true;
    std::strcpy(svc1.short_name.data(), "OLD");

    catalog.update_service(svc1);
    EXPECT_EQ(catalog.service_count(), 1u);

    // Update same service
    Service svc2;
    svc2.service_id = 1001;
    svc2.major_channel = 5;
    svc2.minor_channel = 1;
    svc2.valid = true;
    std::strcpy(svc2.short_name.data(), "NEW");

    catalog.update_service(svc2);
    EXPECT_EQ(catalog.service_count(), 1u);

    const Service* found = catalog.find_service(1001);
    ASSERT_NE(found, nullptr);
    EXPECT_STREQ(found->short_name.data(), "NEW");
}

TEST(ServiceCatalogTest, RemoveService) {
    ServiceCatalog catalog;

    Service svc;
    svc.service_id = 1001;
    svc.valid = true;

    catalog.update_service(svc);
    EXPECT_EQ(catalog.service_count(), 1u);

    bool removed = catalog.remove_service(1001);
    EXPECT_TRUE(removed);
    EXPECT_EQ(catalog.service_count(), 0u);
}

TEST(ServiceCatalogTest, Clear) {
    ServiceCatalog catalog;

    for (int i = 0; i < 5; i++) {
        Service svc;
        svc.service_id = static_cast<uint16_t>(1000 + i);
        svc.valid = true;
        catalog.update_service(svc);
    }

    EXPECT_EQ(catalog.service_count(), 5u);

    catalog.clear();
    EXPECT_EQ(catalog.service_count(), 0u);
}

TEST(ServiceCatalogTest, UpdateCallback) {
    ServiceCatalog catalog;

    int callback_count = 0;
    catalog.set_update_callback([&]() { callback_count++; });

    Service svc;
    svc.service_id = 1001;
    svc.valid = true;

    catalog.update_service(svc);
    EXPECT_EQ(callback_count, 1);

    catalog.update_service(svc);  // Update
    EXPECT_EQ(callback_count, 2);

    catalog.remove_service(1001);
    EXPECT_EQ(callback_count, 3);
}

TEST(ServiceCatalogTest, GetAllServices) {
    ServiceCatalog catalog;

    for (int i = 0; i < 3; i++) {
        Service svc;
        svc.service_id = static_cast<uint16_t>(1000 + i);
        svc.valid = true;
        catalog.update_service(svc);
    }

    auto all = catalog.get_all_services();
    EXPECT_EQ(all.size(), 3u);
}

TEST(ServiceCatalogTest, ServiceComponents) {
    ServiceCatalog catalog;

    Service svc;
    svc.service_id = 1001;
    svc.valid = true;

    // Add video component
    svc.components[0].type = ComponentType::VIDEO;
    svc.components[0].video_codec = VideoCodec::HEVC;
    svc.components[0].width = 1920;
    svc.components[0].height = 1080;
    svc.components[0].valid = true;

    // Add audio component
    svc.components[1].type = ComponentType::AUDIO;
    svc.components[1].audio_codec = AudioCodec::AC4;
    svc.components[1].channels = 6;
    svc.components[1].valid = true;

    svc.num_components = 2;

    catalog.update_service(svc);

    const Service* found = catalog.find_service(1001);
    ASSERT_NE(found, nullptr);

    const ServiceComponent* video = found->get_video();
    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->video_codec, VideoCodec::HEVC);
    EXPECT_EQ(video->width, 1920u);

    const ServiceComponent* audio = found->get_audio();
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->audio_codec, AudioCodec::AC4);
}

//==============================================================================
// RouteParser Tests
//==============================================================================

TEST(RouteParserTest, DefaultConstruction) {
    ServiceCatalog catalog;
    RouteParser parser(&catalog);

    EXPECT_EQ(parser.get_stats().lct_packets_parsed, 0u);
}

TEST(RouteParserTest, ParseSltXml) {
    ServiceCatalog catalog;
    RouteParser parser(&catalog);

    auto packet = create_lct_slt_packet(SAMPLE_SLT_XML);
    bool result = parser.process_lct_packet(packet.data(), packet.size());

    EXPECT_TRUE(result);
    EXPECT_EQ(parser.get_stats().slt_tables_received, 1u);
    EXPECT_EQ(catalog.service_count(), 3u);
}

TEST(RouteParserTest, SltServiceDetails) {
    ServiceCatalog catalog;
    RouteParser parser(&catalog);

    auto packet = create_lct_slt_packet(SAMPLE_SLT_XML);
    parser.process_lct_packet(packet.data(), packet.size());

    const Service* svc1 = catalog.find_service(1001);
    ASSERT_NE(svc1, nullptr);
    EXPECT_EQ(svc1->major_channel, 5u);
    EXPECT_EQ(svc1->minor_channel, 1u);
    EXPECT_STREQ(svc1->short_name.data(), "WXYZ");
    EXPECT_EQ(svc1->service_category, 1u);

    const Service* svc2 = catalog.find_service(1002);
    ASSERT_NE(svc2, nullptr);
    EXPECT_STREQ(svc2->short_name.data(), "WXYZ-2");

    const Service* svc3 = catalog.find_service(1003);
    ASSERT_NE(svc3, nullptr);
    EXPECT_EQ(svc3->service_category, 2u);  // Radio
}

TEST(RouteParserTest, ParseMpdXml) {
    ServiceCatalog catalog;
    RouteParser parser(&catalog);

    auto packet = create_lct_mpd_packet(SAMPLE_MPD_XML, 1001);
    bool result = parser.process_lct_packet(packet.data(), packet.size());

    EXPECT_TRUE(result);
    EXPECT_EQ(parser.get_stats().mpd_received, 1u);

    const MpdInfo* mpd = parser.get_mpd(1001);
    ASSERT_NE(mpd, nullptr);
    EXPECT_TRUE(mpd->valid);
    EXPECT_EQ(mpd->base_url, "http://example.com/stream/");
}

TEST(RouteParserTest, InvalidXml) {
    ServiceCatalog catalog;
    RouteParser parser(&catalog);

    // Create packet with invalid XML
    const char* bad_xml = "<invalid>not an SLT</invalid>";
    auto packet = create_lct_slt_packet(bad_xml);
    bool result = parser.process_lct_packet(packet.data(), packet.size());

    EXPECT_FALSE(result);
    EXPECT_GT(parser.get_stats().xml_errors, 0u);
}

TEST(RouteParserTest, NullInput) {
    ServiceCatalog catalog;
    RouteParser parser(&catalog);

    bool result = parser.process_signaling(nullptr, 100);
    EXPECT_FALSE(result);
}

TEST(RouteParserTest, EmptyInput) {
    ServiceCatalog catalog;
    RouteParser parser(&catalog);

    std::vector<uint8_t> data;
    bool result = parser.process_signaling(data.data(), 0);
    EXPECT_FALSE(result);
}

TEST(RouteParserTest, TruncatedLctHeader) {
    ServiceCatalog catalog;
    RouteParser parser(&catalog);

    // Only 2 bytes - not enough for LCT header
    std::vector<uint8_t> data = {0x10, 0xA0};
    bool result = parser.process_signaling(data.data(), data.size());

    EXPECT_FALSE(result);
}

TEST(RouteParserTest, Reset) {
    ServiceCatalog catalog;
    RouteParser parser(&catalog);

    auto packet = create_lct_slt_packet(SAMPLE_SLT_XML);
    parser.process_lct_packet(packet.data(), packet.size());

    EXPECT_GT(parser.get_stats().lct_packets_parsed, 0u);

    parser.reset();

    EXPECT_EQ(parser.get_stats().lct_packets_parsed, 0u);
    EXPECT_FALSE(parser.get_slt().valid);
}

TEST(RouteParserTest, GetSlt) {
    ServiceCatalog catalog;
    RouteParser parser(&catalog);

    auto packet = create_lct_slt_packet(SAMPLE_SLT_XML);
    parser.process_lct_packet(packet.data(), packet.size());

    const auto& slt = parser.get_slt();
    EXPECT_TRUE(slt.valid);
    EXPECT_EQ(slt.bsid, 1234u);
    EXPECT_EQ(slt.services.size(), 3u);
}

TEST(RouteParserTest, MultipleSltUpdates) {
    ServiceCatalog catalog;
    RouteParser parser(&catalog);

    // First SLT
    auto packet1 = create_lct_slt_packet(SAMPLE_SLT_XML);
    parser.process_lct_packet(packet1.data(), packet1.size());

    EXPECT_EQ(catalog.service_count(), 3u);

    // Second SLT with different services
    const char* slt2 = R"(<?xml version="1.0"?>
<SLT bsid="1234" version="2">
    <Service serviceId="2001" majorChannelNo="6" minorChannelNo="1"
             shortServiceName="NEW" serviceCategory="1"/>
</SLT>
)";
    auto packet2 = create_lct_slt_packet(slt2);
    parser.process_lct_packet(packet2.data(), packet2.size());

    // Should have 4 services now (original 3 + 1 new)
    EXPECT_EQ(catalog.service_count(), 4u);
    EXPECT_NE(catalog.find_service(2001), nullptr);
}

TEST(RouteParserTest, SegmentCallback) {
    ServiceCatalog catalog;
    RouteParser parser(&catalog);

    int callback_count = 0;
    parser.set_segment_callback([&](uint16_t, const DashSegment&) { callback_count++; });

    // Callback is set but not triggered by current implementation
    // (would need full segment delivery parsing)
    EXPECT_EQ(callback_count, 0);
}

//==============================================================================
// LCT Header Parsing Tests
//==============================================================================

TEST(RouteParserTest, LctVersionCheck) {
    ServiceCatalog catalog;
    RouteParser parser(&catalog);

    // Create packet with wrong LCT version
    std::vector<uint8_t> packet = {
        0x00,                                      // version=0 (invalid)
        0xA0, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01,  // TSI
        0x00, 0x00, 0x00, 0x01                     // TOI
    };

    bool result = parser.process_lct_packet(packet.data(), packet.size());
    EXPECT_FALSE(result);
}

//==============================================================================
// Edge Cases
//==============================================================================

TEST(ServiceCatalogTest, MaxServices) {
    ServiceCatalog catalog;

    // Fill catalog to max
    for (size_t i = 0; i < MAX_SERVICES; i++) {
        Service svc;
        svc.service_id = static_cast<uint16_t>(i + 1);
        svc.valid = true;
        EXPECT_TRUE(catalog.update_service(svc));
    }

    EXPECT_EQ(catalog.service_count(), MAX_SERVICES);

    // Try to add one more
    Service extra;
    extra.service_id = 9999;
    extra.valid = true;
    bool result = catalog.update_service(extra);
    EXPECT_FALSE(result);  // Should fail - catalog full
}

TEST(ServiceCatalogTest, InvalidService) {
    ServiceCatalog catalog;

    Service invalid;
    invalid.service_id = 0;  // Invalid ID
    invalid.valid = true;

    bool result = catalog.update_service(invalid);
    EXPECT_FALSE(result);

    Service not_valid;
    not_valid.service_id = 1001;
    not_valid.valid = false;  // Not valid

    result = catalog.update_service(not_valid);
    EXPECT_FALSE(result);
}

TEST(ServiceCatalogTest, FindNonexistent) {
    ServiceCatalog catalog;

    EXPECT_EQ(catalog.find_service(9999), nullptr);
    EXPECT_EQ(catalog.find_by_channel(99, 99), nullptr);
    EXPECT_EQ(catalog.get_service(100), nullptr);
}

}  // namespace
}  // namespace framing
}  // namespace atsc3
