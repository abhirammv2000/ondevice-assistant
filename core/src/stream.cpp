#include "assist/stream.hpp"

#include <utility>

namespace assist {

StreamSession::StreamSession(Engine engine, PartialCallback on_partial)
    : engine_(std::move(engine)), on_partial_(std::move(on_partial)) {
    worker_ = std::thread([this] { run(); });  // started last, when every member it reads exists
}

StreamSession::~StreamSession() {
    cancel();
    if (worker_.joinable()) worker_.join();
}

void StreamSession::update(std::string_view text) {
    {
        std::lock_guard lock(mutex_);
        if (finishing_ || cancelled_) return;
        ++stats_.received;
        ++newest_;
        if (pending_) ++stats_.dropped;  // the worker never got to the previous one, and now never will
        pending_ = std::string(text);
    }
    wake_.notify_one();
}

Result StreamSession::finish() {
    std::unique_lock lock(mutex_);
    if (!finishing_ && !cancelled_) {
        finishing_ = true;
        if (newest_ == 0) {  // nothing was ever said, so the answer is the one for an empty utterance
            pending_ = std::string();
            newest_ = 1;
        }
        wake_.notify_one();
    }
    finished_.wait(lock, [&] { return done_; });
    return final_;
}

void StreamSession::cancel() {
    {
        std::lock_guard lock(mutex_);
        if (done_) return;
        cancelled_ = true;
        pending_.reset();
    }
    wake_.notify_one();
}

StreamSession::Stats StreamSession::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

void StreamSession::run() {
    std::unique_lock lock(mutex_);
    Result last;
    for (;;) {
        wake_.wait(lock, [&] { return pending_.has_value() || cancelled_ || finishing_; });
        if (cancelled_) {
            final_ = Result{};
            final_.reason = "cancelled";
            done_ = true;
            finished_.notify_all();
            return;
        }
        if (!pending_) {
            // finish() was called after the newest hypothesis had already been processed, so that result is the final one
            final_ = std::move(last);
            done_ = true;
            finished_.notify_all();
            return;
        }
        std::string text = std::move(*pending_);
        pending_.reset();
        const std::uint64_t sequence = newest_;

        lock.unlock();
        Result result = engine_.handle(text);  // the slow part runs without the lock, so update() is never blocked by it
        lock.lock();

        ++stats_.processed;
        if (sequence != newest_) {
            ++stats_.stale;  // a newer hypothesis arrived while this one was being processed
            continue;
        }
        if (finishing_) {
            final_ = std::move(result);
            done_ = true;
            finished_.notify_all();
            return;
        }
        last = result;
        if (on_partial_) {
            lock.unlock();
            try {
                on_partial_(result, sequence);
            } catch (...) {
                // a callback that throws must not end the session
            }
            lock.lock();
            ++stats_.delivered;
        }
    }
}

}  // namespace assist
