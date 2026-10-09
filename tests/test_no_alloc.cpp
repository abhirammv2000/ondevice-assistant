// The hot path must not touch the heap. These tests replace operator new (alloc_counter.cpp) and count.
// They are skipped on Windows, where the replacement is not built.
#include <string>
#include <vector>

#include "alloc_counter.hpp"
#include "assist/bounded_queue.hpp"
#include "assist/engine.hpp"
#include "assist/lru.hpp"
#include "assist/spsc_ring.hpp"
#include "assist/tokenizer.hpp"
#include "doctest.h"
#include "model_builder.hpp"

using namespace assist;

#if !defined(_WIN32)

namespace {

const std::vector<std::string> kUtterances = {
    "set a timer for ten minutes", "wake me at 7:30 tomorrow", "call mom", "what is the weather like in san francisco today",
    "", "   ", "caf\xC3\xA9 au lait", std::string(400, 'a') + " b", std::string(600, 'z'), "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15",
    "text sarah saying i am running late please call me when you land and do not forget the keys"};

}  // namespace

TEST_CASE("the counter itself works: a vector allocation is seen") {
    testutil::ScopedAllocCount counter;
    std::vector<int> v(100);
    v[0] = 1;
    // an optimiser may remove an allocation whose result is never used (clang does at -O2), so make the
    // vector's storage observable; otherwise this control could report zero and the checks below mean nothing
    asm volatile("" : : "r"(v.data()) : "memory");
    CHECK(counter.count() >= 1);
}

TEST_CASE("no allocation: turning an utterance into features") {
    TokenScratch scratch;
    extract_features("warm up", scratch);
    testutil::ScopedAllocCount counter;
    for (int i = 0; i < 5000; ++i) {
        for (const auto& u : kUtterances) extract_features(u, scratch);
    }
    const std::size_t allocated = counter.count();
    CHECK(allocated == 0);
}

TEST_CASE("no allocation: scoring features against the model") {
    const auto bytes = testutil::build_trigger_model({"a", "b", "c"}, {{"timer", "a"}, {"call", "b"}});
    Model model;
    REQUIRE(Model::from_bytes(bytes, model) == LoadStatus::Ok);
    auto scratch = model.make_scratch();
    TokenScratch tokens;
    extract_features("set a timer", tokens);
    model.predict(tokens.feature_span(), scratch);

    testutil::ScopedAllocCount counter;
    float sink = 0;
    for (int i = 0; i < 5000; ++i) {
        for (const auto& u : kUtterances) {
            extract_features(u, tokens);
            sink += model.predict(tokens.feature_span(), scratch).confidence;
        }
    }
    const std::size_t allocated = counter.count();
    CHECK(allocated == 0);
    CHECK(sink > 0);
}

TEST_CASE("no allocation: classify() on a whole engine, after the first call") {
    Engine engine(testutil::stub_model(), std::make_shared<FixedClock>(LocalTime{}), {});
    engine.classify("warm up");
    testutil::ScopedAllocCount counter;
    for (int i = 0; i < 5000; ++i) {
        for (const auto& u : kUtterances) engine.classify(u);
    }
    const std::size_t allocated = counter.count();
    CHECK(allocated == 0);
}

TEST_CASE("no allocation: a full LRU cache taking inserts and lookups") {
    LruCache<int, int> cache(64);
    for (int i = 0; i < 64; ++i) cache.put(i, i);
    testutil::ScopedAllocCount counter;
    for (int i = 0; i < 200000; ++i) {
        cache.put(i % 1000, i);
        cache.get((i * 7) % 1000);
    }
    const std::size_t allocated = counter.count();
    CHECK(allocated == 0);
}

TEST_CASE("no allocation: the ring buffer and the bounded queue, for plain values") {
    SpscRing<int, 64> ring;
    BoundedQueue<int> queue(64);
    testutil::ScopedAllocCount counter;
    int out = 0;
    for (int i = 0; i < 100000; ++i) {
        ring.try_push(i);
        ring.try_pop(out);
        queue.try_push(i);
        queue.try_pop();
    }
    const std::size_t allocated = counter.count();
    CHECK(allocated == 0);
}

TEST_CASE("handling a whole request does allocate, because the reply is built as strings (this is documented, not hidden)") {
    Engine engine(testutil::stub_model(), std::make_shared<FixedClock>(LocalTime{2026, 10, 8, 9, 15, 0}), {});
    engine.handle("warm up");
    testutil::ScopedAllocCount counter;
    engine.handle("set a timer for ten minutes");
    CHECK(counter.count() > 0);
}

#else

TEST_CASE("no allocation tests are not built on Windows") { CHECK(true); }

#endif
