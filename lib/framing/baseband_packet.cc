// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// baseband_packet.cc — A/322 Baseband Packet descrambling and header parsing
//
// Reference: ATSC A/322 Section 5.2

#include "baseband_packet.h"

namespace atsc3 {
namespace framing {

namespace {

// Figure 5.6: Galois register X1..X16 (bit k-1 holds X^k), loaded with
// 0xF180 read X16..X1. Each clock X1 <- X16 and X(k+1) <- Xk ^ X16 after
// the taps of G(x) = 1+X+X^3+X^6+X^7+X^11+X^12+X^13+X^16. The randomizing
// byte D7..D0 is X14, X13, X12, X11, X7, X4, X3, X1.
constexpr uint16_t kInit = 0xF180;
constexpr uint16_t kTapsAfter = (1u << 0) | (1u << 2) | (1u << 5) | (1u << 6) | (1u << 10) |
                                (1u << 11) | (1u << 12);  // after X1, X3, X6, X7, X11, X12, X13
// X16 fed back into X1 and into the stage after each tap.
constexpr uint16_t kFeedback = static_cast<uint16_t>((kTapsAfter << 1) | 1u);
constexpr int kOutBit[8] = {0, 2, 3, 6, 10, 11, 12, 13};  // D0..D7 as X-index - 1

}  // namespace

void bbp_scramble(uint8_t* bytes, size_t num_bytes) {
    uint16_t x = kInit;
    for (size_t i = 0; i < num_bytes; i++) {
        uint8_t r = 0;
        for (int d = 0; d < 8; d++) {
            r |= static_cast<uint8_t>(((x >> kOutBit[d]) & 1u) << d);
        }
        bytes[i] ^= r;
        uint16_t fb = (x >> 15) & 1u;
        x = static_cast<uint16_t>(x << 1);
        if (fb) {
            x ^= kFeedback;
        }
    }
}

void pack_bits_msb_first(const uint8_t* bits, size_t num_bits, uint8_t* bytes) {
    for (size_t i = 0; i < num_bits / 8; i++) {
        uint8_t b = 0;
        for (size_t k = 0; k < 8; k++) {
            b = static_cast<uint8_t>((b << 1) | (bits[8 * i + k] & 1u));
        }
        bytes[i] = b;
    }
}

bool parse_bbp_header(const uint8_t* bbp, size_t len, BbpHeader* h) {
    *h = BbpHeader{};
    if (len < 1) {
        return false;
    }
    const uint8_t pointer_lsb = bbp[0] & 0x7F;
    if ((bbp[0] & 0x80) == 0) {
        h->pointer = pointer_lsb;
        h->header_length = 1;
    } else {
        if (len < 2) {
            return false;
        }
        h->pointer = static_cast<uint16_t>(((bbp[1] >> 2) << 7) | pointer_lsb);
        h->ofi = bbp[1] & 0x03;
        h->header_length = 2;
        if (h->ofi != 0) {
            if (len < 3) {
                return false;
            }
            h->ext_type = bbp[2] >> 5;
            h->ext_length = bbp[2] & 0x1F;
            h->header_length = 3;
            if (h->ofi != 1) {  // long and mixed: 13-bit EXT_LEN
                if (len < 4) {
                    return false;
                }
                h->ext_length |= static_cast<size_t>(bbp[3]) << 5;
                h->header_length = 4;
            }
            const size_t ext_start = h->header_length;
            h->header_length += h->ext_length;
            if (h->ofi != 3 && h->ext_type == 0) {
                if (h->ext_length < 2 || ext_start + 2 > len) {
                    return false;
                }
                h->has_counter = true;
                h->counter = static_cast<uint16_t>((bbp[ext_start] << 8) | bbp[ext_start + 1]);
            }
        }
    }
    if (h->header_length > len) {
        return false;
    }
    const size_t payload = len - h->header_length;
    return h->pointer == kBbpNoAlpStart || h->pointer < payload;
}

bool BbpDepacketizer::push(const uint8_t* bbp, size_t len, bool valid, BbpPayload* out) {
    *out = BbpPayload{};
    BbpHeader h;
    if (!valid || !parse_bbp_header(bbp, len, &h)) {
        in_sync_ = false;
        return false;
    }
    const size_t payload_len = len - h.header_length;
    if (in_sync_) {
        out->offset = h.header_length;
        out->length = payload_len;
        return true;
    }
    if (h.pointer == kBbpNoAlpStart) {
        return true;  // still waiting for an ALP packet start
    }
    in_sync_ = true;
    out->offset = h.header_length + h.pointer;
    out->length = payload_len - h.pointer;
    out->resync = true;
    return true;
}

}  // namespace framing
}  // namespace atsc3
