#include <list>
#include <map>
#include <optional>
#include <string>

#include "assist/lru.hpp"
#include "doctest.h"

using namespace assist;

namespace {

// every key lands in one of four buckets, so the probe sequences are long and overlap, which is where a mistake in
// the delete-by-shifting code would show
struct CollidingHash {
    std::size_t operator()(int k) const noexcept { return static_cast<std::size_t>(k) % 4; }
};

// the obvious slow implementation to compare against
class ReferenceLru {
public:
    explicit ReferenceLru(std::size_t cap) : cap_(cap) {}
    std::optional<int> get(int k) {
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->first == k) {
                items_.splice(items_.begin(), items_, it);
                return it->second;
            }
        }
        return std::nullopt;
    }
    void put(int k, int v) {
        if (cap_ == 0) return;
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->first == k) {
                it->second = v;
                items_.splice(items_.begin(), items_, it);
                return;
            }
        }
        if (items_.size() == cap_) items_.pop_back();
        items_.emplace_front(k, v);
    }
    std::size_t size() const { return items_.size(); }

private:
    std::size_t cap_;
    std::list<std::pair<int, int>> items_;
};

}  // namespace

TEST_CASE("lru: stores and finds values") {
    LruCache<std::string, int> c(3);
    c.put("a", 1);
    c.put("b", 2);
    REQUIRE(c.get("a") != nullptr);
    CHECK(*c.get("a") == 1);
    CHECK(*c.get("b") == 2);
    CHECK(c.get("missing") == nullptr);
    CHECK(c.size() == 2);
}

TEST_CASE("lru: the least recently used entry is evicted first") {
    LruCache<std::string, int> c(3);
    c.put("a", 1);
    c.put("b", 2);
    c.put("c", 3);
    c.get("a");        // a is now the freshest, so b is the oldest
    c.put("d", 4);
    CHECK(c.get("b") == nullptr);
    CHECK(c.get("a") != nullptr);
    CHECK(c.get("c") != nullptr);
    CHECK(c.get("d") != nullptr);
    CHECK(c.size() == 3);
}

TEST_CASE("lru: putting an existing key replaces the value and refreshes it") {
    LruCache<std::string, int> c(2);
    c.put("a", 1);
    c.put("b", 2);
    c.put("a", 10);   // a is freshest again
    c.put("c", 3);    // evicts b
    CHECK(c.get("b") == nullptr);
    CHECK(*c.get("a") == 10);
    CHECK(c.size() == 2);
}

TEST_CASE("lru: capacity one and capacity zero") {
    LruCache<int, int> one(1);
    one.put(1, 1);
    one.put(2, 2);
    CHECK(one.get(1) == nullptr);
    CHECK(*one.get(2) == 2);

    LruCache<int, int> zero(0);
    zero.put(1, 1);
    CHECK(zero.get(1) == nullptr);
    CHECK(zero.size() == 0);
}

TEST_CASE("lru: hit and miss counters, and clear") {
    LruCache<int, int> c(2);
    c.put(1, 1);
    c.get(1);
    c.get(1);
    c.get(2);
    CHECK(c.hits() == 2);
    CHECK(c.misses() == 1);
    c.clear();
    CHECK(c.size() == 0);
    CHECK(c.get(1) == nullptr);
    c.put(5, 5);
    CHECK(*c.get(5) == 5);
}

TEST_CASE("lru: behaves exactly like the slow reference over many random operations, even when every key collides") {
    for (const std::size_t capacity : {1U, 2U, 3U, 7U, 16U}) {
        LruCache<int, int, CollidingHash> fast(capacity);
        ReferenceLru slow(capacity);
        std::uint32_t state = 777;
        auto next = [&] {
            state = state * 1664525U + 1013904223U;
            return state >> 8;
        };
        for (int i = 0; i < 50000; ++i) {
            const int key = static_cast<int>(next() % 40);
            if (next() % 3 == 0) {
                const int value = static_cast<int>(next());
                fast.put(key, value);
                slow.put(key, value);
            } else {
                const int* got = fast.get(key);
                const auto want = slow.get(key);
                REQUIRE((got != nullptr) == want.has_value());
                if (got) REQUIRE(*got == *want);
            }
            REQUIRE(fast.size() == slow.size());
        }
    }
}
