// fuzz_alp_demux.cc — libFuzzer harness for ALP demultiplexer
//
// Fuzzes the ALP packet parser with arbitrary byte sequences to find:
//   - Buffer overflows in header parsing
//   - Integer overflows in length calculations
//   - Out-of-bounds reads in payload handling
//   - Assertion failures in state machine
//
// Usage:
//   ./fuzz_alp_demux corpus/alp/ -max_len=65536

#include <cstddef>
#include <cstdint>
#include <framing/alp_demux.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Skip empty inputs
    if (size == 0) {
        return 0;
    }

    // Create demux with default config (CRC checking enabled)
    atsc3::framing::AlpDemuxConfig config;
    config.check_crc = false;  // Disable CRC for faster fuzzing
    config.max_datagram_size = 65535;

    atsc3::framing::AlpDemux demux(config);

    // Register callbacks to ensure callback paths are exercised
    bool ip_received = false;
    bool signaling_received = false;
    bool ts_received = false;

    demux.set_ip_callback([&ip_received](uint8_t /*plp_id*/, const uint8_t* /*data*/,
                                         size_t /*len*/) { ip_received = true; });

    demux.set_signaling_callback(
        [&signaling_received](uint8_t /*plp_id*/, const uint8_t* /*data*/, size_t /*len*/) {
            signaling_received = true;
        });

    demux.set_ts_callback([&ts_received](uint8_t /*plp_id*/, const uint8_t* /*data*/,
                                         size_t /*len*/) { ts_received = true; });

    // Use first byte as PLP ID if available (randomizes PLP handling)
    uint8_t plp_id = (size > 1) ? data[0] % 4 : 0;
    const uint8_t* payload = (size > 1) ? data + 1 : data;
    size_t payload_len = (size > 1) ? size - 1 : size;

    // Process the fuzzed input
    demux.process(plp_id, payload, payload_len);

    // Also test flush path
    demux.flush();

    // Test reset path
    demux.reset();

    // Process again after reset (tests state cleanup)
    if (payload_len > 0) {
        demux.process(plp_id, payload, payload_len);
    }

    return 0;
}
