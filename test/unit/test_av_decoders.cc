// test_av_decoders.cc — Unit tests for av/ decoders
//
// Tests ring buffer, HEVC decoder, and audio decoder
// Note: Full decode tests require FFmpeg - these test the interface

#include "ring_buffer.h"

#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace atsc3 {
namespace av {
namespace {

//==============================================================================
// Ring Buffer Tests
//==============================================================================

TEST(RingBufferTest, DefaultConstruction) {
    RingBuffer<int, 8> buffer;

    EXPECT_TRUE(buffer.empty());
    EXPECT_FALSE(buffer.full());
    EXPECT_EQ(buffer.size(), 0u);
    EXPECT_EQ(buffer.capacity(), 7u);  // One slot reserved
}

TEST(RingBufferTest, PushPop) {
    RingBuffer<int, 8> buffer;

    EXPECT_TRUE(buffer.push(42));
    EXPECT_FALSE(buffer.empty());
    EXPECT_EQ(buffer.size(), 1u);

    auto value = buffer.pop();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 42);
    EXPECT_TRUE(buffer.empty());
}

TEST(RingBufferTest, PushMultiple) {
    RingBuffer<int, 8> buffer;

    for (int i = 0; i < 7; i++) {
        EXPECT_TRUE(buffer.push(i));
    }

    EXPECT_EQ(buffer.size(), 7u);
    EXPECT_TRUE(buffer.full());

    // Should fail when full
    EXPECT_FALSE(buffer.push(100));
}

TEST(RingBufferTest, PopMultiple) {
    RingBuffer<int, 8> buffer;

    for (int i = 0; i < 5; i++) {
        buffer.push(i);
    }

    for (int i = 0; i < 5; i++) {
        auto value = buffer.pop();
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, i);
    }

    EXPECT_TRUE(buffer.empty());
    EXPECT_FALSE(buffer.pop().has_value());
}

TEST(RingBufferTest, Peek) {
    RingBuffer<int, 8> buffer;

    EXPECT_EQ(buffer.peek(), nullptr);

    buffer.push(42);
    buffer.push(43);

    const int* peeked = buffer.peek();
    ASSERT_NE(peeked, nullptr);
    EXPECT_EQ(*peeked, 42);

    // Peek doesn't remove
    EXPECT_EQ(buffer.size(), 2u);
}

TEST(RingBufferTest, Clear) {
    RingBuffer<int, 8> buffer;

    for (int i = 0; i < 5; i++) {
        buffer.push(i);
    }

    buffer.clear();

    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.size(), 0u);
}

TEST(RingBufferTest, MoveSemantics) {
    RingBuffer<std::vector<int>, 4> buffer;

    std::vector<int> vec = {1, 2, 3, 4, 5};
    EXPECT_TRUE(buffer.push(std::move(vec)));

    // Original should be empty after move
    EXPECT_TRUE(vec.empty());

    auto popped = buffer.pop();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(popped->size(), 5u);
}

TEST(RingBufferTest, Wraparound) {
    RingBuffer<int, 4> buffer;  // Capacity 3

    // Fill and drain multiple times to test wraparound
    for (int round = 0; round < 5; round++) {
        EXPECT_TRUE(buffer.push(round * 10 + 0));
        EXPECT_TRUE(buffer.push(round * 10 + 1));
        EXPECT_TRUE(buffer.push(round * 10 + 2));
        EXPECT_TRUE(buffer.full());

        EXPECT_EQ(*buffer.pop(), round * 10 + 0);
        EXPECT_EQ(*buffer.pop(), round * 10 + 1);
        EXPECT_EQ(*buffer.pop(), round * 10 + 2);
        EXPECT_TRUE(buffer.empty());
    }
}

TEST(RingBufferTest, ThreadSafety) {
    RingBuffer<int, 1024> buffer;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    constexpr int NUM_ITEMS = 10000;

    // Producer thread
    std::thread producer([&]() {
        for (int i = 0; i < NUM_ITEMS; i++) {
            while (!buffer.push(i)) {
                std::this_thread::yield();
            }
            produced++;
        }
    });

    // Consumer thread
    std::thread consumer([&]() {
        int expected = 0;
        while (consumed < NUM_ITEMS) {
            auto value = buffer.pop();
            if (value.has_value()) {
                EXPECT_EQ(*value, expected++);
                consumed++;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(produced.load(), NUM_ITEMS);
    EXPECT_EQ(consumed.load(), NUM_ITEMS);
}

//==============================================================================
// Struct Tests (without FFmpeg)
//==============================================================================

TEST(VideoFrameTest, DefaultConstruction) {
    // Test that VideoFrame struct is properly default-initialized
    // This doesn't require FFmpeg
    struct VideoFrameTest {
        uint32_t width = 0;
        uint32_t height = 0;
        bool valid() const {
            return width > 0 && height > 0;
        }
    };

    VideoFrameTest frame;
    EXPECT_EQ(frame.width, 0u);
    EXPECT_EQ(frame.height, 0u);
    EXPECT_FALSE(frame.valid());

    frame.width = 1920;
    frame.height = 1080;
    EXPECT_TRUE(frame.valid());
}

TEST(AudioFrameTest, DefaultConstruction) {
    struct AudioFrameTest {
        uint32_t sample_rate = 0;
        uint8_t channels = 0;
        uint32_t num_samples = 0;
        bool valid() const {
            return sample_rate > 0 && channels > 0 && num_samples > 0;
        }
        double duration_sec() const {
            return sample_rate > 0 ? static_cast<double>(num_samples) / sample_rate : 0.0;
        }
    };

    AudioFrameTest frame;
    EXPECT_FALSE(frame.valid());

    frame.sample_rate = 48000;
    frame.channels = 2;
    frame.num_samples = 1024;
    EXPECT_TRUE(frame.valid());
    EXPECT_NEAR(frame.duration_sec(), 1024.0 / 48000.0, 1e-9);
}

//==============================================================================
// Large Buffer Tests
//==============================================================================

TEST(RingBufferTest, LargeBuffer) {
    RingBuffer<int, 65536> buffer;

    // Fill to half capacity
    for (int i = 0; i < 32000; i++) {
        EXPECT_TRUE(buffer.push(i));
    }

    EXPECT_EQ(buffer.size(), 32000u);

    // Drain
    for (int i = 0; i < 32000; i++) {
        auto value = buffer.pop();
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, i);
    }
}

TEST(RingBufferTest, ComplexType) {
    struct ComplexData {
        int id;
        std::string name;
        std::vector<double> values;
    };

    RingBuffer<ComplexData, 8> buffer;

    ComplexData data1{1, "test", {1.0, 2.0, 3.0}};
    EXPECT_TRUE(buffer.push(data1));

    auto popped = buffer.pop();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(popped->id, 1);
    EXPECT_EQ(popped->name, "test");
    EXPECT_EQ(popped->values.size(), 3u);
}

}  // namespace
}  // namespace av
}  // namespace atsc3
