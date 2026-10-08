// A bounded multi-producer multi-consumer queue that blocks, with close().
//
// Bounded because an unbounded queue turns a burst of requests into unbounded memory, and on a device the right answer
// to "too much work" is to say so (try_push returns false) and let the caller decide, not to keep accepting until
// the process is killed. The storage is one array allocated up front, so pushing and popping never allocate.
//
// close() is how shutdown works. It wakes every waiting thread. After it, push fails, and pop keeps returning what
// is left and then returns nothing, so a worker can drain the queue and then exit its loop without any separate flag.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace assist {

template <class T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : slots_(capacity == 0 ? 1 : capacity) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    std::size_t capacity() const noexcept { return slots_.size(); }

    // Add without waiting. False when the queue is full or closed.
    bool try_push(T value) {
        std::lock_guard lock(mutex_);
        if (closed_ || count_ == slots_.size()) return false;
        put(std::move(value));
        not_empty_.notify_one();
        return true;
    }

    // Wait for room, then add. False if the queue is closed before there is room.
    bool push(T value) {
        std::unique_lock lock(mutex_);
        not_full_.wait(lock, [&] { return closed_ || count_ < slots_.size(); });
        if (closed_) return false;
        put(std::move(value));
        not_empty_.notify_one();
        return true;
    }

    // Wait for an item. Nothing means the queue is closed and empty.
    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [&] { return closed_ || count_ > 0; });
        if (count_ == 0) return std::nullopt;
        T value = take();
        not_full_.notify_one();
        return value;
    }

    std::optional<T> try_pop() {
        std::lock_guard lock(mutex_);
        if (count_ == 0) return std::nullopt;
        T value = take();
        not_full_.notify_one();
        return value;
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return count_;
    }

    bool closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

private:
    void put(T&& value) {
        slots_[(head_ + count_) % slots_.size()] = std::move(value);
        ++count_;
    }

    T take() {
        T value = std::move(*slots_[head_]);
        slots_[head_].reset();
        head_ = (head_ + 1) % slots_.size();
        --count_;
        return value;
    }

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::vector<std::optional<T>> slots_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    bool closed_ = false;
};

}  // namespace assist
