#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "assist/bounded_queue.hpp"
#include "assist/service.hpp"
#include "assist/spsc_ring.hpp"
#include "assist/stream.hpp"
#include "doctest.h"
#include "model_builder.hpp"

using namespace assist;
using namespace std::chrono_literals;

namespace {

const LocalTime kNow{2026, 10, 8, 9, 15, 0};

Engine make_engine() {
    EngineConfig config;
    config.random_seed = 99;
    Engine e(testutil::stub_model(), std::make_shared<FixedClock>(kNow), config);
    e.set_contacts({{1, "Mom"}, {2, "Dana"}});
    return e;
}

// A gate a test can hold shut so a worker stays busy while the test fills the queue behind it.
class Gate {
public:
    void wait() {
        std::unique_lock lock(m_);
        cv_.wait(lock, [&] { return open_; });
    }
    void open() {
        {
            std::lock_guard lock(m_);
            open_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool open_ = false;
};

}  // namespace

// BoundedQueue

TEST_CASE("queue: first in, first out, and a full queue refuses without blocking") {
    BoundedQueue<int> q(3);
    CHECK(q.try_push(1));
    CHECK(q.try_push(2));
    CHECK(q.try_push(3));
    CHECK_FALSE(q.try_push(4));
    CHECK(q.size() == 3);
    CHECK(*q.try_pop() == 1);
    CHECK(q.try_push(4));
    CHECK(*q.pop() == 2);
    CHECK(*q.pop() == 3);
    CHECK(*q.pop() == 4);
    CHECK_FALSE(q.try_pop().has_value());
}

TEST_CASE("queue: it wraps around its storage many times without losing order") {
    BoundedQueue<int> q(4);
    int next_in = 0, next_out = 0;
    for (int round = 0; round < 1000; ++round) {
        for (int i = 0; i < 3; ++i) REQUIRE(q.try_push(next_in++));
        for (int i = 0; i < 3; ++i) REQUIRE(*q.try_pop() == next_out++);
    }
}

TEST_CASE("queue: pop waits for an item and push waits for room") {
    BoundedQueue<int> q(1);
    auto popped = std::async(std::launch::async, [&] { return q.pop(); });
    std::this_thread::sleep_for(20ms);
    CHECK(q.try_push(7));
    CHECK(*popped.get() == 7);

    q.try_push(1);
    auto pushed = std::async(std::launch::async, [&] { return q.push(2); });
    std::this_thread::sleep_for(20ms);
    CHECK(*q.pop() == 1);
    CHECK(pushed.get());
    CHECK(*q.pop() == 2);
}

TEST_CASE("queue: close wakes a blocked pop, refuses new items and lets the rest drain") {
    BoundedQueue<int> q(4);
    auto blocked = std::async(std::launch::async, [&] { return q.pop(); });
    std::this_thread::sleep_for(20ms);
    q.close();
    CHECK_FALSE(blocked.get().has_value());
    CHECK_FALSE(q.try_push(1));
    CHECK_FALSE(q.push(1));
    CHECK(q.closed());

    BoundedQueue<int> r(4);
    r.try_push(1);
    r.try_push(2);
    r.close();
    CHECK(*r.pop() == 1);
    CHECK(*r.pop() == 2);
    CHECK_FALSE(r.pop().has_value());
}

TEST_CASE("queue: close wakes a producer blocked on a full queue") {
    BoundedQueue<int> q(1);
    q.try_push(1);
    auto blocked = std::async(std::launch::async, [&] { return q.push(2); });
    std::this_thread::sleep_for(20ms);
    q.close();
    CHECK_FALSE(blocked.get());
}

TEST_CASE("queue: four producers and four consumers move every item exactly once") {
    BoundedQueue<int> q(16);
    constexpr int kPerProducer = 20000;
    constexpr int kProducers = 4;
    std::atomic<long long> sum{0};
    std::atomic<int> count{0};
    std::atomic<int> refused{0};  // checked on the main thread: assertion macros are not used from other threads
    std::vector<std::thread> consumers;
    for (int c = 0; c < 4; ++c) {
        consumers.emplace_back([&] {
            while (auto v = q.pop()) {
                sum += *v;
                ++count;
            }
        });
    }
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                if (!q.push(p * kPerProducer + i)) ++refused;
            }
        });
    }
    for (auto& t : producers) t.join();
    q.close();
    for (auto& t : consumers) t.join();
    const long long n = static_cast<long long>(kPerProducer) * kProducers;
    CHECK(refused == 0);
    CHECK(count == n);
    CHECK(sum == n * (n - 1) / 2);
}

TEST_CASE("queue: it holds move-only values") {
    BoundedQueue<std::unique_ptr<int>> q(2);
    CHECK(q.try_push(std::make_unique<int>(5)));
    auto v = q.pop();
    REQUIRE(v.has_value());
    CHECK(**v == 5);
}

// SpscRing

TEST_CASE("ring: push, pop, full and empty") {
    SpscRing<int, 4> r;
    int out = 0;
    CHECK_FALSE(r.try_pop(out));
    for (int i = 0; i < 4; ++i) CHECK(r.try_push(i));
    CHECK_FALSE(r.try_push(99));
    CHECK(r.size_approx() == 4);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(r.try_pop(out));
        CHECK(out == i);
    }
    CHECK_FALSE(r.try_pop(out));
}

TEST_CASE("ring: an index that has wrapped many times still works") {
    SpscRing<int, 8> r;
    int out = 0;
    for (int i = 0; i < 100000; ++i) {
        REQUIRE(r.try_push(i));
        REQUIRE(r.try_pop(out));
        REQUIRE(out == i);
    }
}

TEST_CASE("ring: a producer thread and a consumer thread see every item once, in order") {
    SpscRing<std::uint64_t, 64> r;
    constexpr std::uint64_t kCount = 300000;
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!r.try_push(i)) std::this_thread::yield();
        }
    });
    std::uint64_t expected = 0;
    bool in_order = true;
    while (expected < kCount) {
        std::uint64_t v;
        if (r.try_pop(v)) {
            in_order = in_order && v == expected;
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    CHECK(in_order);
}

TEST_CASE("ring: it carries strings and releases them") {
    SpscRing<std::string, 4> r;
    CHECK(r.try_push(std::string(100, 'x')));
    std::string out;
    CHECK(r.try_pop(out));
    CHECK(out == std::string(100, 'x'));
}

// AssistService

TEST_CASE("service: many requests from many threads all get the right answer") {
    AssistService service(make_engine(), {4, 256});
    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;
    std::atomic<int> right{0}, wrong{0}, not_accepted{0};
    std::vector<std::thread> clients;
    for (int t = 0; t < kThreads; ++t) {
        clients.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                const bool timer = i % 2 == 0;
                const std::string text = timer ? "set a timer for ten minutes" : "call mom";
                const std::string want = timer ? "Timer set for 10 minutes." : "Calling Mom.";
                AssistService::SubmitStatus status;
                do {
                    status = service.submit(text, [&, want](AssistService::Outcome&& o) {
                        (o.kind == AssistService::Outcome::Kind::Done && o.result.reply == want ? right : wrong)++;
                    });
                    if (status == AssistService::SubmitStatus::Busy) std::this_thread::yield();
                } while (status == AssistService::SubmitStatus::Busy);
                if (status != AssistService::SubmitStatus::Accepted) ++not_accepted;
            }
        });
    }
    for (auto& c : clients) c.join();
    service.shutdown();
    CHECK(not_accepted == 0);
    CHECK(right == kThreads * kPerThread);
    CHECK(wrong == 0);
    const auto s = service.stats();
    CHECK(s.accepted == static_cast<std::uint64_t>(kThreads * kPerThread));
    CHECK(s.completed == s.accepted);
    CHECK(s.queue_depth == 0);
}

TEST_CASE("service: a full queue says Busy at once, and everything that was accepted still finishes") {
    AssistService service(make_engine(), {1, 2});
    Gate gate;
    std::atomic<int> finished{0};
    auto count = [&](AssistService::Outcome&&) { ++finished; };

    // the first request holds the only worker inside its callback, so the queue behind it fills
    REQUIRE(service.submit("call mom", [&](AssistService::Outcome&&) { gate.wait(); ++finished; }) == AssistService::SubmitStatus::Accepted);
    std::this_thread::sleep_for(50ms);  // let the worker pick it up
    CHECK(service.submit("call mom", count) == AssistService::SubmitStatus::Accepted);
    CHECK(service.submit("call mom", count) == AssistService::SubmitStatus::Accepted);
    CHECK(service.submit("call mom", count) == AssistService::SubmitStatus::Busy);
    CHECK(service.submit("call mom", count) == AssistService::SubmitStatus::Busy);
    CHECK(service.stats().rejected_busy == 2);

    gate.open();
    service.shutdown();
    CHECK(finished == 3);  // the refused ones were never called back
}

TEST_CASE("service: a request that is late is not run, and the callback says so") {
    AssistService service(make_engine(), {1, 8});
    Gate gate;
    std::promise<AssistService::Outcome> late;
    service.submit("call mom", [&](AssistService::Outcome&&) { gate.wait(); });
    std::this_thread::sleep_for(30ms);
    service.submit("call mom", [&](AssistService::Outcome&& o) { late.set_value(std::move(o)); }, AssistService::SteadyClock::now() + 10ms);
    std::this_thread::sleep_for(60ms);  // the deadline passes while the worker is busy
    gate.open();
    const auto o = late.get_future().get();
    CHECK(o.kind == AssistService::Outcome::Kind::Expired);
    CHECK(o.result.intent.empty());  // the engine never ran
    service.shutdown();
    CHECK(service.stats().expired == 1);
}

TEST_CASE("service: a request cancelled before a worker reaches it is not run") {
    AssistService service(make_engine(), {1, 8});
    Gate gate;
    service.submit("call mom", [&](AssistService::Outcome&&) { gate.wait(); });
    std::this_thread::sleep_for(30ms);
    CancelSource cancel;
    std::promise<AssistService::Outcome> promise;
    service.submit("call mom", [&](AssistService::Outcome&& o) { promise.set_value(std::move(o)); },
                   AssistService::SteadyClock::time_point::max(), cancel.token());
    cancel.cancel();
    gate.open();
    CHECK(promise.get_future().get().kind == AssistService::Outcome::Kind::Cancelled);
    service.shutdown();
    CHECK(service.stats().cancelled == 1);
}

TEST_CASE("service: shutdown runs what is queued, refuses new work, and can be called twice") {
    AssistService service(make_engine(), {2, 100});
    std::atomic<int> done{0};
    for (int i = 0; i < 50; ++i) {
        REQUIRE(service.submit("set a timer for ten minutes", [&](AssistService::Outcome&&) { ++done; }) == AssistService::SubmitStatus::Accepted);
    }
    service.shutdown();
    CHECK(done == 50);
    CHECK(service.submit("call mom", {}) == AssistService::SubmitStatus::Stopped);
    service.shutdown();
}

TEST_CASE("service: destroying it with work still queued is safe") {
    std::atomic<int> done{0};
    {
        AssistService service(make_engine(), {2, 100});
        for (int i = 0; i < 80; ++i) service.submit("call mom", [&](AssistService::Outcome&&) { ++done; });
    }
    CHECK(done == 80);
}

TEST_CASE("service: a callback that throws does not stop the worker") {
    AssistService service(make_engine(), {1, 8});
    std::promise<void> second;
    service.submit("call mom", [](AssistService::Outcome&&) { throw std::runtime_error("callback failed"); });
    service.submit("call mom", [&](AssistService::Outcome&&) { second.set_value(); });
    CHECK(second.get_future().wait_for(2s) == std::future_status::ready);
}

TEST_CASE("service: queue and run times are measured") {
    AssistService service(make_engine(), {1, 8});
    std::promise<AssistService::Outcome> p;
    service.submit("set a timer for ten minutes", [&](AssistService::Outcome&& o) { p.set_value(std::move(o)); });
    const auto o = p.get_future().get();
    CHECK(o.kind == AssistService::Outcome::Kind::Done);
    CHECK(o.run_time.count() > 0);
    CHECK(o.queue_time.count() >= 0);
}

// StreamSession

TEST_CASE("stream: the final result is the one for the newest text") {
    StreamSession session(make_engine());
    session.update("set a");
    session.update("set a timer");
    session.update("set a timer for ten minutes");
    const Result r = session.finish();
    CHECK(r.reply == "Timer set for 10 minutes.");
    CHECK(session.finish().reply == r.reply);  // asking again gives the same answer
}

TEST_CASE("stream: every update is accounted for as processed or dropped") {
    StreamSession session(make_engine());
    for (int i = 0; i < 2000; ++i) session.update(i % 2 ? "call mom" : "set a timer for ten minutes");
    session.update("call dana");
    const Result r = session.finish();
    CHECK(r.reply == "Calling Dana.");
    const auto s = session.stats();
    CHECK(s.received == 2001);
    CHECK(s.received == s.processed + s.dropped);
}

TEST_CASE("stream: a slow listener makes the session drop old hypotheses instead of falling behind") {
    std::atomic<std::uint64_t> last_seen{0};
    std::atomic<bool> in_order{true};
    StreamSession session(make_engine(), [&](const Result&, std::uint64_t seq) {
        if (seq <= last_seen) in_order = false;
        last_seen = seq;
        std::this_thread::sleep_for(5ms);  // slower than the producer
    });
    for (int i = 0; i < 200; ++i) {
        session.update("set a timer for ten minutes");
        std::this_thread::sleep_for(100us);
    }
    session.finish();
    const auto s = session.stats();
    CHECK(in_order);
    CHECK(s.dropped > 0);
    CHECK(s.received == s.processed + s.dropped);
    CHECK(s.delivered <= s.processed);
}

TEST_CASE("stream: partial results arrive while the user is still talking") {
    std::promise<Result> first;
    std::atomic<bool> got{false};
    StreamSession session(make_engine(), [&](const Result& r, std::uint64_t) {
        if (!got.exchange(true)) first.set_value(r);
    });
    session.update("set a timer for ten minutes");
    auto fut = first.get_future();
    REQUIRE(fut.wait_for(2s) == std::future_status::ready);
    CHECK(fut.get().action == "timer.set");
    session.finish();
}

TEST_CASE("stream: finishing with nothing said is the empty-utterance answer") {
    StreamSession session(make_engine());
    const Result r = session.finish();
    CHECK(r.route == Route::Escalate);
    CHECK(r.reason == "empty_utterance");
}

TEST_CASE("stream: cancel ends the session, wakes finish, and later updates are ignored") {
    StreamSession session(make_engine());
    session.update("call mom");
    session.cancel();
    const Result r = session.finish();
    CHECK(r.reason == "cancelled");
    session.update("set a timer");
    CHECK(session.stats().received <= 1);
}

TEST_CASE("stream: updates after finish are ignored") {
    StreamSession session(make_engine());
    session.update("call mom");
    session.finish();
    session.update("call dana");
    CHECK(session.stats().received == 1);
}

TEST_CASE("stream: a callback that throws does not end the session") {
    StreamSession session(make_engine(), [](const Result&, std::uint64_t) { throw std::runtime_error("boom"); });
    session.update("call mom");
    session.update("call dana");
    CHECK(session.finish().reply == "Calling Dana.");
}

TEST_CASE("stream: a hundred sessions at once, each finishing, each destroyed cleanly") {
    std::vector<std::thread> threads;
    std::atomic<int> right{0};
    for (int s = 0; s < 100; ++s) {
        threads.emplace_back([&] {
            StreamSession session(make_engine());
            for (int i = 0; i < 20; ++i) session.update("set a timer");
            session.update("call mom");
            right += session.finish().reply == "Calling Mom.";
        });
    }
    for (auto& t : threads) t.join();
    CHECK(right == 100);
}

TEST_CASE("stream: destroying a session that was never finished is safe") {
    for (int i = 0; i < 50; ++i) {
        StreamSession session(make_engine());
        session.update("call mom");
    }
    CHECK(true);
}
