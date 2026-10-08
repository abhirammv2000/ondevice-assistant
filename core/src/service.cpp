#include "assist/service.hpp"

#include <utility>

namespace assist {

AssistService::AssistService(const Engine& prototype, Options options) : queue_(options.queue_capacity) {
    const std::size_t n = options.workers == 0 ? 1 : options.workers;
    workers_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        // each worker gets its own engine, made here on the constructing thread so no engine is touched by two threads
        workers_.emplace_back([this, engine = prototype.fork()]() mutable { work(std::move(engine)); });
    }
}

AssistService::~AssistService() { shutdown(); }

AssistService::SubmitStatus AssistService::submit(std::string utterance, Callback callback, SteadyClock::time_point deadline,
                                                  std::shared_ptr<std::atomic<bool>> cancel) {
    if (stopped_.load(std::memory_order_acquire)) return SubmitStatus::Stopped;
    Job job{std::move(utterance), std::move(callback), deadline, std::move(cancel), SteadyClock::now()};
    if (!queue_.try_push(std::move(job))) {
        // either full or closed by a shutdown that began after the check above
        if (queue_.closed()) return SubmitStatus::Stopped;
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return SubmitStatus::Busy;
    }
    accepted_.fetch_add(1, std::memory_order_relaxed);
    return SubmitStatus::Accepted;
}

void AssistService::work(Engine engine) {
    while (auto job = queue_.pop()) {
        Outcome out;
        const auto started = SteadyClock::now();
        out.queue_time = std::chrono::duration_cast<std::chrono::nanoseconds>(started - job->queued_at);
        if (job->cancel && job->cancel->load(std::memory_order_relaxed)) {
            out.kind = Outcome::Kind::Cancelled;
            cancelled_.fetch_add(1, std::memory_order_relaxed);
        } else if (started > job->deadline) {
            out.kind = Outcome::Kind::Expired;
            expired_.fetch_add(1, std::memory_order_relaxed);
        } else {
            out.result = engine.handle(job->utterance);
            out.run_time = std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now() - started);
            completed_.fetch_add(1, std::memory_order_relaxed);
        }
        try {
            if (job->callback) job->callback(std::move(out));
        } catch (...) {
            // a callback that throws must not take the worker down with it
        }
    }
}

void AssistService::shutdown() {
    stopped_.store(true, std::memory_order_release);
    queue_.close();  // workers drain what is queued, then pop() returns nothing and they leave their loops
    for (std::thread& t : workers_) {
        if (t.joinable()) t.join();
    }
}

AssistService::Stats AssistService::stats() const {
    Stats s;
    s.accepted = accepted_.load(std::memory_order_relaxed);
    s.completed = completed_.load(std::memory_order_relaxed);
    s.rejected_busy = rejected_.load(std::memory_order_relaxed);
    s.expired = expired_.load(std::memory_order_relaxed);
    s.cancelled = cancelled_.load(std::memory_order_relaxed);
    s.queue_depth = queue_.size();
    return s;
}

}  // namespace assist
