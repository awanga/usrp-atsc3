// fuzz_route_parser.cc — libFuzzer harness for ROUTE parser
//
// Fuzzes the ROUTE signaling parser with arbitrary byte sequences to find:
//   - XML parsing vulnerabilities
//   - Buffer overflows in LCT header parsing
//   - Integer overflows in length/offset calculations
//   - Memory corruption in object reassembly
//
// Usage:
//   ./fuzz_route_parser corpus/route/ -max_len=1048576

#include <cstddef>
#include <cstdint>
#include <framing/route_parser.h>
#include <framing/service_catalog.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Skip empty inputs
    if (size == 0) {
        return 0;
    }

    // Create service catalog and parser
    atsc3::framing::ServiceCatalog catalog;

    atsc3::framing::RouteParserConfig config;
    config.max_xml_size = 1024 * 1024;  // 1MB max XML
    config.verbose = false;

    atsc3::framing::RouteParser parser(&catalog, config);

    // Register segment callback to exercise callback paths
    bool segment_received = false;
    parser.set_segment_callback(
        [&segment_received](uint16_t /*service_id*/, const atsc3::framing::DashSegment& /*seg*/) {
            segment_received = true;
        });

    // Try processing as signaling data
    parser.process_signaling(data, size);

    // Also try processing as LCT packet directly
    parser.process_lct_packet(data, size);

    // Test reset path
    parser.reset();

    // Process again after reset
    if (size > 0) {
        parser.process_signaling(data, size);
    }

    // Verify catalog didn't get corrupted
    (void)catalog.service_count();

    return 0;
}
