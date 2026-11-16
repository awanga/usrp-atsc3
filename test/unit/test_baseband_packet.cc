// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: 2026 Alfred Wanga
//
// test_baseband_packet.cc — Unit tests for lib/framing/baseband_packet.h
//
// Expected values from A/322 Section 5.2: the first scrambling-sequence
// bytes printed in 5.2.3, the pointer = 130 header example of 5.2.2.1, the
// OFI = 01 / EXT_LEN = 5 counter example of 5.2.2.3.1, and a bit-array model
// of the Figure 5.6 shift register. The depacketizer is checked against a
// transmitter model that maps an ALP byte stream into BBPs per 5.2.1.

#include "framing/baseband_packet.h"

#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace atsc3 {
namespace framing {
namespace {

TEST(BbpScrambleTest, FirstBytesMatchSection523) {
    // "1100 0000 0110 1101 0011 1111 ..."
    std::vector<uint8_t> zeros(3, 0);
    bbp_scramble(zeros.data(), zeros.size());
    EXPECT_EQ(zeros, (std::vector<uint8_t>{0xC0, 0x6D, 0x3F}));
}

// Figure 5.6 cell by cell: X1..X16, initial 0xF180 (X16 first), XOR after
// X1, X3, X6, X7, X11, X12, X13; outputs D0..D7 at X1, X3, X4, X7, X11, X12,
// X13, X14.
std::vector<uint8_t> figure56_sequence(size_t n) {
    int x[17] = {};
    const char* init = "1111000110000000";  // X16 .. X1
    for (int k = 16; k >= 1; k--) {
        x[k] = init[16 - k] - '0';
    }
    const int taps[] = {1, 3, 6, 7, 11, 12, 13};
    const int outs[] = {1, 3, 4, 7, 11, 12, 13, 14};
    std::vector<uint8_t> seq;
    for (size_t i = 0; i < n; i++) {
        int b = 0;
        for (int d = 7; d >= 0; d--) {
            b = (b << 1) | x[outs[d]];
        }
        seq.push_back(static_cast<uint8_t>(b));
        int fb = x[16];
        int nx[17] = {};
        nx[1] = fb;
        for (int k = 1; k <= 15; k++) {
            int t = 0;
            for (int tap : taps) {
                t |= tap == k;
            }
            nx[k + 1] = x[k] ^ (t ? fb : 0);
        }
        for (int k = 1; k <= 16; k++) {
            x[k] = nx[k];
        }
    }
    return seq;
}

TEST(BbpScrambleTest, MatchesFigure56RegisterAndInverts) {
    std::vector<uint8_t> data(7000);
    std::mt19937 rng(1);
    for (auto& b : data) {
        b = static_cast<uint8_t>(rng());
    }
    auto seq = figure56_sequence(data.size());
    std::vector<uint8_t> s(data);
    bbp_scramble(s.data(), s.size());
    for (size_t i = 0; i < data.size(); i++) {
        ASSERT_EQ(s[i], data[i] ^ seq[i]) << "byte " << i;
    }
    bbp_scramble(s.data(), s.size());
    EXPECT_EQ(s, data);
}

TEST(BbpPackTest, MsbFirst) {
    const uint8_t bits[16] = {1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 1};
    uint8_t bytes[2];
    pack_bits_msb_first(bits, 16, bytes);
    EXPECT_EQ(bytes[0], 0xC0);
    EXPECT_EQ(bytes[1], 0x6D);
}

TEST(BbpHeaderTest, OneByteBaseField) {
    std::vector<uint8_t> bbp(100, 0xAA);
    bbp[0] = 0x05;  // MODE 0, pointer 5
    BbpHeader h;
    ASSERT_TRUE(parse_bbp_header(bbp.data(), bbp.size(), &h));
    EXPECT_EQ(h.pointer, 5);
    EXPECT_EQ(h.header_length, 1u);
}

TEST(BbpHeaderTest, Pointer130ExampleOfSection5221) {
    std::vector<uint8_t> bbp(300, 0);
    bbp[0] = 0x82;  // 10000010
    bbp[1] = 0x04;  // 00000100
    BbpHeader h;
    ASSERT_TRUE(parse_bbp_header(bbp.data(), bbp.size(), &h));
    EXPECT_EQ(h.pointer, 130);
    EXPECT_EQ(h.ofi, 0);
    EXPECT_EQ(h.header_length, 2u);
}

TEST(BbpHeaderTest, CounterExampleOfSection52231) {
    // OFI = 01, EXT_TYPE = 000, EXT_LEN = 5: 2 + 1 + 5 = 8 header bytes.
    std::vector<uint8_t> bbp(200, 0);
    bbp[0] = 0x80;              // MODE 1, pointer LSB 0
    bbp[1] = 0x01;              // pointer MSB 0, OFI 01
    bbp[2] = 0x05;              // EXT_TYPE 000, EXT_LEN 5
    bbp[3] = 0x12;              // counter MSB
    bbp[4] = 0x34;              // counter LSB
    BbpHeader h;
    ASSERT_TRUE(parse_bbp_header(bbp.data(), bbp.size(), &h));
    EXPECT_EQ(h.header_length, 8u);
    EXPECT_TRUE(h.has_counter);
    EXPECT_EQ(h.counter, 0x1234);
}

TEST(BbpHeaderTest, LongAndMixedExtensionLengths) {
    std::vector<uint8_t> bbp(1000, 0);
    bbp[0] = 0x80 | 3;           // pointer 3
    bbp[1] = 0x02;               // OFI 10 (long)
    bbp[2] = (7 << 5) | (300 & 31);  // EXT_TYPE 111 (padding), EXT_LEN LSB
    bbp[3] = 300 >> 5;           // EXT_LEN MSB
    BbpHeader h;
    ASSERT_TRUE(parse_bbp_header(bbp.data(), bbp.size(), &h));
    EXPECT_EQ(h.ext_length, 300u);
    EXPECT_EQ(h.header_length, 304u);
    EXPECT_FALSE(h.has_counter);
    EXPECT_EQ(h.pointer, 3);

    bbp[1] = 0x03;               // OFI 11 (mixed): NUM_EXT then 13-bit EXT_LEN
    bbp[2] = (2 << 5) | 8;
    bbp[3] = 0;
    ASSERT_TRUE(parse_bbp_header(bbp.data(), bbp.size(), &h));
    EXPECT_EQ(h.ext_type, 2);
    EXPECT_EQ(h.header_length, 12u);
}

TEST(BbpHeaderTest, RejectsMalformedHeaders) {
    std::vector<uint8_t> bbp(10, 0);
    BbpHeader h;
    bbp[0] = 0x80;
    bbp[1] = 0x01;
    bbp[2] = 0x1F;  // short ext of 31 bytes in a 10-byte packet
    EXPECT_FALSE(parse_bbp_header(bbp.data(), bbp.size(), &h));
    bbp[2] = 0x01;  // counter extension shorter than 2 bytes
    EXPECT_FALSE(parse_bbp_header(bbp.data(), bbp.size(), &h));
    bbp[0] = 0x09;  // MODE 0, pointer 9 >= 9-byte payload
    EXPECT_FALSE(parse_bbp_header(bbp.data(), bbp.size(), &h));
    bbp[0] = 0xFF;  // pointer 8191 (no ALP start)
    bbp[1] = 0xFC;
    EXPECT_TRUE(parse_bbp_header(bbp.data(), bbp.size(), &h));
    EXPECT_EQ(h.pointer, kBbpNoAlpStart);
}

// Section 5.2.1 transmitter: ALP packets (here opaque byte runs) fill BBP
// payloads in order, split across BBPs; each header is a 2-byte Base Field
// with the pointer to the first packet starting in that BBP.
std::vector<std::vector<uint8_t>> make_bbps(const std::vector<std::vector<uint8_t>>& packets,
                                            size_t bbp_len) {
    std::vector<uint8_t> stream;
    std::vector<size_t> starts;
    for (const auto& p : packets) {
        starts.push_back(stream.size());
        stream.insert(stream.end(), p.begin(), p.end());
    }
    const size_t payload = bbp_len - 2;
    std::vector<std::vector<uint8_t>> bbps;
    for (size_t base = 0; base + payload <= stream.size(); base += payload) {
        size_t pointer = kBbpNoAlpStart;
        for (size_t s : starts) {
            if (s >= base && s < base + payload) {
                pointer = s - base;
                break;
            }
        }
        std::vector<uint8_t> bbp(bbp_len);
        bbp[0] = static_cast<uint8_t>(0x80 | (pointer & 0x7F));
        bbp[1] = static_cast<uint8_t>((pointer >> 7) << 2);
        std::copy(stream.begin() + static_cast<std::ptrdiff_t>(base),
                  stream.begin() + static_cast<std::ptrdiff_t>(base + payload), bbp.begin() + 2);
        bbps.push_back(bbp);
    }
    return bbps;
}

TEST(BbpDepacketizerTest, RebuildsAlpStreamAndResyncsAfterLoss) {
    std::mt19937 rng(9);
    std::vector<std::vector<uint8_t>> packets;
    for (int i = 0; i < 60; i++) {
        // Some packets longer than a BBP, so some BBPs have no start.
        size_t n = (i % 7 == 3) ? 900 : 20 + rng() % 200;
        packets.push_back(std::vector<uint8_t>(n, static_cast<uint8_t>(i)));
    }
    auto bbps = make_bbps(packets, 400);
    ASSERT_GT(bbps.size(), 10u);

    BbpDepacketizer d;
    std::vector<uint8_t> stream;
    size_t resyncs = 0;
    const size_t lost = 5;
    for (size_t i = 0; i < bbps.size(); i++) {
        BbpPayload out;
        bool ok = d.push(bbps[i].data(), bbps[i].size(), i != lost, &out);
        EXPECT_EQ(ok, i != lost);
        if (out.resync) {
            resyncs++;
            stream.clear();
        }
        stream.insert(stream.end(), bbps[i].begin() + static_cast<std::ptrdiff_t>(out.offset),
                      bbps[i].begin() + static_cast<std::ptrdiff_t>(out.offset + out.length));
    }
    EXPECT_EQ(resyncs, 2u);  // first packet start, and again after the loss
    // After the last resync the stream is whole packets from a packet start.
    ASSERT_FALSE(stream.empty());
    size_t first = stream[0];
    size_t pos = 0;
    for (size_t p = first; p < packets.size() && pos + packets[p].size() <= stream.size(); p++) {
        ASSERT_TRUE(std::equal(packets[p].begin(), packets[p].end(),
                               stream.begin() + static_cast<std::ptrdiff_t>(pos)))
            << "packet " << p;
        pos += packets[p].size();
    }
    EXPECT_GT(pos, stream.size() / 2);
}

}  // namespace
}  // namespace framing
}  // namespace atsc3
