// Many requests at once: a fixed pool of worker threads in front of the engine.
//
// What it is built to do when there is too much work, because that is where the design shows:
//   * the queue is bounded. A full queue makes submit() return Busy immediately, so the caller can fall back to
//     sending the request elsewhere or telling the user, instead of waiting behind work it cannot see.
//   * a request carries a deadline. A worker that picks up a request after its deadline does not run it, because
//     the answer would arrive after the user stopped waiting, and the callback is told it expired.
//   * a request can be cancelled before a worker starts on it.
//   * shutdown() stops accepting new work, finishes what is already queued, and joins the threads. The destructor
//     calls it, so a service cannot be destroyed with threads still running.
//
// Each worker owns its own Engine (a fork of the prototype), because an Engine keeps scratch space and a cache and is
// not safe to share. They share the model and the contact list, which are read-only.
//
// The callback runs on a worker thread. If it throws, the exception is swallowed so one bad callback cannot kill a worker.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "assist/bounded_queue.hpp"
#include "assist/engine.hpp"

namespace assist {

// A request is cancelled through the flag this holds. Copies share the flag, so keep one and give out the token.
class CancelSource {
public:
    CancelSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    void cancel() noexcept { flag_->store(true, std::memory_order_relaxed); }
    std::shared_ptr<std::atomic<bool>> token() const noexcept { return flag_; }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

class AssistService {
public:
    using SteadyClock = std::chrono::steady_clock;

    enum class SubmitStatus { Accepted, Busy, Stopped };

    struct Outcome {
        enum class Kind { Done, Expired, Cancelled };
        Kind kind = Kind::Done;
        Result result;                       // only for Done
        std::chrono::nanoseconds queue_time{};  // waiting for a worker
        std::chrono::nanoseconds run_time{};    // the engine
    };
    using Callback = std::function<void(Outcome&&)>;

    struct Options {
        std::size_t workers = 2;
        std::size_t queue_capacity = 64;
    };

    struct Stats {
        std::uint64_t accepted = 0;
        std::uint64_t completed = 0;
        std::uint64_t rejected_busy = 0;
        std::uint64_t expired = 0;
        std::uint64_t cancelled = 0;
        std::size_t queue_depth = 0;
    };

    AssistService(const Engine& prototype, Options options);
    ~AssistService();
    AssistService(const AssistService&) = delete;
    AssistService& operator=(const AssistService&) = delete;

    // Queue a request. The callback is called exactly once for an accepted request, whether it ran, expired or was
    // cancelled, and never for one that was refused.
    SubmitStatus submit(std::string utterance, Callback callback, SteadyClock::time_point deadline = SteadyClock::time_point::max(),
                        std::shared_ptr<std::atomic<bool>> cancel = nullptr);

    // Stop accepting, run what is queued, and wait for the workers. Safe to call more than once.
    void shutdown();

    Stats stats() const;

private:
    struct Job {
        std::string utterance;
        Callback callback;
        SteadyClock::time_point deadline;
        std::shared_ptr<std::atomic<bool>> cancel;
        SteadyClock::time_point queued_at;
    };

    void work(Engine engine);

    BoundedQueue<Job> queue_;
    std::vector<std::thread> workers_;
    std::atomic<std::uint64_t> accepted_{0}, completed_{0}, rejected_{0}, expired_{0}, cancelled_{0};
    std::atomic<bool> stopped_{false};
};

}  // namespace assist
