// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga

#pragma once

// Baseband Packets — receive side of A/322 Section 5.2
//
// Each FEC Frame carries one Baseband Packet (BBP) of Kpayload bits
// (Tables 6.1/6.2), scrambled as a whole (5.2.3). A BBP is a header (Base
// Field, optional Optional and Extension Fields; padding lives in the
// Extension Field) followed by a payload of ALP packet bytes. ALP packets
// may be split across BBPs; the Base Field pointer gives the offset of the
// first ALP packet that starts in the payload (8191: none starts).
//
// AXI4-S Interface Contract:
//   Input:  TDATA=uint8 (BBP byte, descrambled) TVALID TREADY TLAST(BBP)
//   Output: TDATA=uint8 (ALP byte stream)       TVALID TREADY TUSER(resync)
//
// Reference: ATSC A/322 Section 5.2, Tables 5.1, 5.2

#include <cstddef>
#include <cstdint>

namespace atsc3 {
namespace framing {

// XOR bytes with the Section 5.2.3 scrambling sequence, restarted at the
// first byte (one call per BBP). Scrambling and descrambling are the same.
void bbp_scramble(uint8_t* bytes, size_t num_bytes);

// Pack bits (one per byte, 0/1, first bit = MSB of the first byte) into
// num_bits/8 bytes. num_bits must be a multiple of 8 (every Kpayload is).
void pack_bits_msb_first(const uint8_t* bits, size_t num_bits, uint8_t* bytes);

constexpr uint16_t kBbpNoAlpStart = 8191;

struct BbpHeader {
    uint16_t pointer = 0;      // offset of the first ALP packet start in the payload
    uint8_t ofi = 0;           // Table 5.1 (0 if MODE = 0)
    size_t header_length = 0;  // Base + Optional + Extension Field bytes
    uint8_t ext_type = 0;      // Table 5.2 (short/long modes); NUM_EXT (mixed mode)
    size_t ext_length = 0;     // EXT_LEN in bytes
    bool has_counter = false;  // EXT_TYPE 000 in short/long mode
    uint16_t counter = 0;
};

// Returns false if the header runs past len, the counter extension is
// shorter than its 2 bytes, or the pointer lies beyond the payload.
bool parse_bbp_header(const uint8_t* bbp, size_t len, BbpHeader* header);

// Which bytes of a BBP continue the ALP byte stream.
struct BbpPayload {
    size_t offset = 0;  // into the BBP
    size_t length = 0;
    bool resync = false;  // discard any partial ALP packet held downstream first
};

// Tracks ALP packet alignment across BBPs of one PLP.
class BbpDepacketizer {
public:
    // bbp: one descrambled BBP. valid = false marks a BBP that failed outer
    // decoding; it is dropped and alignment is lost until a BBP with an
    // ALP packet start. Returns false (and an empty slice) for a dropped or
    // malformed BBP.
    bool push(const uint8_t* bbp, size_t len, bool valid, BbpPayload* out);

    void reset() {
        in_sync_ = false;
    }

    bool in_sync() const {
        return in_sync_;
    }

private:
    bool in_sync_ = false;
};

}  // namespace framing
}  // namespace atsc3
