#pragma once

// Ring Buffer — Thread-safe circular buffer for A/V frames
//
// Lock-free single-producer/single-consumer ring buffer for
// passing decoded frames between decoder threads and playback.
//
// Design constraints for RTL portability:
//   - Fixed capacity (no dynamic allocation after construction)
//   - Power-of-2 size for efficient modulo
//   - Atomic operations for thread safety

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace atsc3 {
namespace av {

// Ring buffer for arbitrary types
// Capacity must be power of 2
template <typename T, size_t Capacity>
class RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static_assert(Capacity > 0, "Capacity must be non-zero");

public:
    RingBuffer() : head_(0), tail_(0) {}

    // Push element (producer side)
    // Returns true if successful, false if full
    bool push(const T& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next_head = (head + 1) & (Capacity - 1);

        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;  // Full
        }

        buffer_[head] = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    // Push with move semantics
    bool push(T&& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next_head = (head + 1) & (Capacity - 1);

        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;  // Full
        }

        buffer_[head] = std::move(item);
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    // Pop element (consumer side)
    // Returns nullopt if empty
    std::optional<T> pop() {
        size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt;  // Empty
        }

        T item = std::move(buffer_[tail]);
        tail_.store((tail + 1) & (Capacity - 1), std::memory_order_release);
        return item;
    }

    // Peek at front element without removing
    const T* peek() const {
        size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire)) {
            return nullptr;  // Empty
        }

        return &buffer_[tail];
    }

    // Check if empty
    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    // Check if full
    bool full() const {
        size_t head = head_.load(std::memory_order_acquire);
        size_t next_head = (head + 1) & (Capacity - 1);
        return next_head == tail_.load(std::memory_order_acquire);
    }

    // Current number of elements
    size_t size() const {
        size_t head = head_.load(std::memory_order_acquire);
        size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & (Capacity - 1);
    }

    // Maximum capacity
    constexpr size_t capacity() const {
        return Capacity - 1;  // One slot always empty to distinguish full/empty
    }

    // Clear all elements
    void clear() {
        tail_.store(head_.load(std::memory_order_relaxed), std::memory_order_release);
    }

private:
    std::array<T, Capacity> buffer_;
    std::atomic<size_t> head_;  // Write position
    std::atomic<size_t> tail_;  // Read position
};

}  // namespace av
}  // namespace atsc3
