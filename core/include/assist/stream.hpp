// Understanding speech while it is still being recognised.
//
// A speech recogniser does not hand over a finished sentence. It sends a growing, sometimes revised text: "set", "set
// a timer", "set a timer for ten", "set a timer for ten minutes". Waiting for the end wastes the time the user spent
// talking, so the engine works on each hypothesis as it arrives and the host can show or prepare the likely action
// before the user has finished.
//
// The recogniser is faster than the engine at its peak, so hypotheses can pile up. Only the newest one matters, so
// update() replaces any hypothesis the worker has not started on, and the older text is counted as dropped and never
// processed. A result that finishes after a newer hypothesis has arrived is stale and is not delivered either.
//
// finish() says the user has stopped. It waits for the newest text to be processed and returns that result, which is
// the final one. cancel() abandons the session. Both end the worker thread, and so does the destructor.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "assist/engine.hpp"

namespace assist {

class StreamSession {
public:
    // Called on the worker thread with the result for a hypothesis that was still the newest when it finished.
    // `sequence` counts the calls to update(), starting at 1.
    using PartialCallback = std::function<void(const Result&, std::uint64_t sequence)>;

    struct Stats {
        std::uint64_t received = 0;   // calls to update()
        std::uint64_t processed = 0;  // hypotheses the engine ran on
        std::uint64_t delivered = 0;  // partial results passed to the callback
        std::uint64_t dropped = 0;    // received but replaced by a newer one before the engine reached them
        std::uint64_t stale = 0;      // processed but out of date by the time they finished
    };

    explicit StreamSession(Engine engine, PartialCallback on_partial = {});
    ~StreamSession();
    StreamSession(const StreamSession&) = delete;
    StreamSession& operator=(const StreamSession&) = delete;

    // Offer the newest hypothesis. Ignored after finish() or cancel().
    void update(std::string_view text);

    // The result for the newest hypothesis, once it has been processed. Call from one thread. Calling it again
    // returns the same result. With no update() at all, the result is for an empty utterance.
    Result finish();

    // Abandon the session. A finish() that is waiting returns an empty-utterance escalation.
    void cancel();

    Stats stats() const;

private:
    void run();

    Engine engine_;
    PartialCallback on_partial_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;       // the worker waits here for work
    std::condition_variable finished_;   // finish() waits here for the final result
    std::optional<std::string> pending_;
    std::uint64_t newest_ = 0;           // sequence number of the newest hypothesis
    bool finishing_ = false;
    bool cancelled_ = false;
    bool done_ = false;
    Result final_;
    Stats stats_;
    std::thread worker_;
};

}  // namespace assist
