// A lock-free ring buffer for exactly one producer thread and exactly one consumer thread.
//
// The speech recogniser produces text on one thread and the engine consumes it on another, which is the shape this
// is for. With one writer per end there is nothing to lock: the producer owns `head_` and the consumer owns `tail_`,
// each reads the other's with an acquire load and publishes its own with a release store. The release on the
// producer's store makes the element it just wrote visible to a consumer that sees the new head. The same holds
// the other way for a slot being freed.
//
// Two details that matter for speed:
//   * head_ and tail_ sit on different cache lines. If they shared one, each thread's write would keep invalidating
//     the other's copy of the line (false sharing) although they never touch the same variable.
//   * each side keeps a stale copy of the other's index and only re-reads the real one when the stale copy says the
//     ring looks full or empty. That turns most operations into plain loads from the thread's own cache line.
// bench/bench.cpp measures both against a mutex-protected queue.
//
// It is wrong to call try_push from two threads at once, or try_pop from two threads at once. There is no check.
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <utility>

namespace assist {

// 64 bytes on the x86-64 and Arm cores this runs on (Apple's performance cores use 128, which wastes a little space and
// is still correct). std::hardware_destructive_interference_size is avoided because it changes with compiler flags,
// which makes a header that uses it an ABI hazard.
inline constexpr std::size_t kCacheLine = 64;

template <class T, std::size_t N>
class SpscRing {
    static_assert(N >= 2 && std::has_single_bit(N), "the capacity must be a power of two, so an index wraps with a mask");

public:
    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    static constexpr std::size_t capacity() noexcept { return N; }

    bool try_push(T value) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_seen_ == N) {
            tail_seen_ = tail_.load(std::memory_order_acquire);  // look again at where the consumer really is
            if (head - tail_seen_ == N) return false;
        }
        slots_[head & (N - 1)] = std::move(value);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_seen_) {
            head_seen_ = head_.load(std::memory_order_acquire);
            if (tail == head_seen_) return false;
        }
        out = std::move(slots_[tail & (N - 1)]);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // A snapshot. It can be stale by the time the caller looks at it, and is meant for tests and metrics.
    std::size_t size_approx() const noexcept { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }

private:
    // the producer's line: its index and its stale copy of the consumer's
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    std::size_t tail_seen_ = 0;
    // the consumer's line
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    std::size_t head_seen_ = 0;
    alignas(kCacheLine) T slots_[N];
};

}  // namespace assist
