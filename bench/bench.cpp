// Micro-benchmarks for the pieces that sit on the request path, each next to the obvious simple version of the same
// thing, so a number says how much a design choice is worth and not only how fast the result is.
//
//   assist_bench --model models/clinc150.pmodel [--float-model clinc150_float.pmodel] [--only NAME] [--json out.json]
//
// A timer call costs 20 to 30 ns, which is as much as some of these operations, so each measurement times a batch of
// calls and divides. The batch is repeated and the table reports the median and the 99th percentile of the
// per-call time across batches, which shows how steady the speed is and not only how fast it can go.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "assist/bounded_queue.hpp"
#include "assist/engine.hpp"
#include "assist/fuzzy.hpp"
#include "assist/hash.hpp"
#include "assist/lru.hpp"
#include "assist/model.hpp"
#include "assist/service.hpp"
#include "assist/spsc_ring.hpp"
#include "assist/tokenizer.hpp"

using namespace assist;
using Steady = std::chrono::steady_clock;

namespace {

template <class T>
inline void keep(const T& value) {
    asm volatile("" : : "g"(&value) : "memory");  // stops the compiler from deleting work whose result is unused
}

struct Row {
    std::string name;
    double median_ns, p99_ns;
    double per_second;
};
std::vector<Row> g_rows;
std::map<std::string, double> g_extra;

template <class F>
Row measure(const std::string& name, F&& fn, std::size_t batch, std::size_t rounds = 200) {
    for (std::size_t i = 0; i < batch; ++i) fn(i);  // warm up caches and branch predictors
    std::vector<double> per_call;
    per_call.reserve(rounds);
    for (std::size_t r = 0; r < rounds; ++r) {
        const auto t0 = Steady::now();
        for (std::size_t i = 0; i < batch; ++i) fn(i);
        const auto t1 = Steady::now();
        per_call.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(batch));
    }
    std::sort(per_call.begin(), per_call.end());
    Row row{name, per_call[per_call.size() / 2], per_call[per_call.size() * 99 / 100], 0};
    row.per_second = 1e9 / row.median_ns;
    g_rows.push_back(row);
    std::printf("  %-52s %10.1f ns   p99 %10.1f ns   %12.0f /s\n", name.c_str(), row.median_ns, row.p99_ns, row.per_second);
    return row;
}

void section(const char* title) { std::printf("\n%s\n", title); }

const std::vector<std::string> kUtterances = {
    "set a timer for ten minutes", "wake me up at seven thirty tomorrow morning", "call mom", "what is the weather like in san francisco today",
    "remind me to buy milk at five pm", "text sarah that i am running late", "what is twelve times seven", "play some music by bob dylan",
    "how much is 250 dollars in euros", "tell me a joke", "what time is it", "translate good morning into spanish"};

// the simple versions

// what most people write first: strings, vectors of strings, and a set to remove duplicates
std::vector<std::uint32_t> naive_features(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : text) {
        const bool token = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '\'' || static_cast<unsigned char>(c) >= 0x80;
        if (token) {
            current.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
        } else if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) tokens.push_back(current);
    for (auto& t : tokens) {
        if (std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; })) t = "<num>";
    }
    std::set<std::uint32_t> unique;
    auto hash = [](const std::string& s) {
        std::uint64_t h = kFnvOffset;
        for (unsigned char c : s) {
            h ^= c;
            h *= kFnvPrime;
        }
        return fold32(h);
    };
    for (const auto& t : tokens) unique.insert(hash("u:" + t));
    if (!tokens.empty()) {
        unique.insert(hash("b:^ " + tokens.front()));
        for (std::size_t i = 0; i + 1 < tokens.size(); ++i) unique.insert(hash("b:" + tokens[i] + " " + tokens[i + 1]));
        unique.insert(hash("b:" + tokens.back() + " $"));
    }
    return {unique.begin(), unique.end()};
}

// std::unordered_map plus std::list, the textbook LRU
class NaiveLru {
public:
    explicit NaiveLru(std::size_t capacity) : capacity_(capacity) {}
    int* get(int key) {
        const auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        order_.splice(order_.begin(), order_, it->second);
        return &it->second->second;
    }
    void put(int key, int value) {
        if (const auto it = map_.find(key); it != map_.end()) {
            it->second->second = value;
            order_.splice(order_.begin(), order_, it->second);
            return;
        }
        if (order_.size() == capacity_) {
            map_.erase(order_.back().first);
            order_.pop_back();
        }
        order_.emplace_front(key, value);
        map_[key] = order_.begin();
    }

private:
    std::size_t capacity_;
    std::list<std::pair<int, int>> order_;
    std::unordered_map<int, std::list<std::pair<int, int>>::iterator> map_;
};

// an SPSC ring with the two indices next to each other and every operation reading the other side's index
template <class T, std::size_t N>
class NaiveSpsc {
public:
    bool try_push(T v) {
        const auto h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == N) return false;
        slots_[h & (N - 1)] = std::move(v);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool try_pop(T& out) {
        const auto t = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) == t) return false;
        out = std::move(slots_[t & (N - 1)]);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

private:
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    T slots_[N];
};

// the whole name distance between the query and every name, which is what a first version does
int full_distance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= b.size(); ++j) cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::vector<std::string> make_names(std::size_t n) {
    static const char* first[] = {"ann", "ben", "cara", "dan", "eva", "finn", "gus", "hana", "ivan", "jade", "kyle", "lena", "mia", "noah", "olga", "pete"};
    static const char* last[] = {"lopez", "nguyen", "oneil", "patel", "quinn", "rossi", "singh", "tran", "ueda", "vega", "wong", "xu", "young", "zhao"};
    std::mt19937 rng(7);
    std::vector<std::string> names;
    names.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        names.push_back(std::string(first[rng() % 16]) + " " + last[rng() % 14] + std::to_string(rng() % 1000));
    }
    return names;
}

// throughput of a producer thread feeding a consumer thread through any queue that has try_push and try_pop
template <class Ring>
double pipe_throughput(Ring& ring, std::uint64_t count) {
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < count; ++i) {
            while (!ring.try_push(i)) std::this_thread::yield();
        }
    });
    const auto t0 = Steady::now();
    std::uint64_t got = 0, v = 0;
    while (got < count) {
        if (ring.try_pop(v)) ++got;
    }
    const auto t1 = Steady::now();
    producer.join();
    return static_cast<double>(count) / std::chrono::duration<double>(t1 - t0).count();
}

template <class Q>
struct TryPopAdapter {
    Q& q;
    bool try_push(std::uint64_t v) { return q.try_push(v); }
    bool try_pop(std::uint64_t& out) {
        auto v = q.try_pop();
        if (!v) return false;
        out = *v;
        return true;
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string model_path = "models/clinc150.pmodel", float_path, only, json_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--model") model_path = next();
        else if (a == "--float-model") float_path = next();
        else if (a == "--only") only = next();
        else if (a == "--json") json_path = next();
    }
    auto wanted = [&](const char* name) { return only.empty() || only == name; };

    Model model;
    if (const auto s = Model::load(model_path, model, false); s != LoadStatus::Ok) {
        std::fprintf(stderr, "cannot load %s: %s\n", model_path.c_str(), to_string(s));
        return 1;
    }
    std::printf("model: %s, %u classes, %u hashed features, %.2f MB, %s\n", model_path.c_str(), model.class_count(), model.feature_count(),
                static_cast<double>(model.file_bytes()) / 1e6, model.quantized() ? "int8" : "float32");

    if (wanted("features")) {
        section("turning text into features (12 different utterances, cycled)");
        TokenScratch scratch;
        measure("tokenizer: fixed buffers, no allocation", [&](std::size_t i) { extract_features(kUtterances[i % kUtterances.size()], scratch); keep(scratch); }, 2000);
        measure("tokenizer: strings, vectors and a std::set", [&](std::size_t i) { auto f = naive_features(kUtterances[i % kUtterances.size()]); keep(f); }, 2000);
    }

    if (wanted("model")) {
        section("scoring one utterance against 150 classes");
        TokenScratch scratch;
        auto sc = model.make_scratch();
        std::vector<std::vector<std::uint32_t>> feats;
        for (const auto& u : kUtterances) {
            extract_features(u, scratch);
            feats.emplace_back(scratch.features, scratch.features + scratch.feature_count);
        }
        measure(model.quantized() ? "predict: int8 weights" : "predict: float32 weights", [&](std::size_t i) { auto p = model.predict(feats[i % feats.size()], sc); keep(p); }, 2000);
        if (!float_path.empty()) {
            Model fm;
            if (Model::load(float_path, fm, false) == LoadStatus::Ok) {
                auto fs = fm.make_scratch();
                measure("predict: float32 weights", [&](std::size_t i) { auto p = fm.predict(feats[i % feats.size()], fs); keep(p); }, 2000);
            }
        }
        measure("classify: tokenize and predict", [&](std::size_t i) {
            extract_features(kUtterances[i % kUtterances.size()], scratch);
            auto p = model.predict(scratch.feature_span(), sc);
            keep(p);
        }, 2000);
    }

    if (wanted("engine")) {
        section("handling whole requests (this part allocates: it builds the reply)");
        EngineConfig config;
        config.random_seed = 1;
        Model m2;
        Model::load(model_path, m2, false);
        Engine engine(std::move(m2), std::make_shared<FixedClock>(LocalTime{2026, 10, 8, 9, 15, 0}), config);
        std::vector<std::pair<std::uint32_t, std::string>> contacts;
        const auto names = make_names(500);
        for (std::size_t i = 0; i < names.size(); ++i) contacts.emplace_back(static_cast<std::uint32_t>(i), names[i]);
        contacts.emplace_back(9999, "Mom");
        engine.set_contacts(contacts);
        for (const char* u : {"set a timer for ten minutes", "wake me at 7:30 tomorrow", "call mom", "what is twelve times seven", "tell me a joke about my email jane@x.com"}) {
            measure(std::string("handle: ") + u, [&](std::size_t) { auto r = engine.handle(u); keep(r); }, 200);
        }
    }

    if (wanted("contacts")) {
        section("finding a contact among N names");
        for (const std::size_t n : {1000UL, 10000UL}) {
            const auto names = make_names(n);
            FuzzyIndex index;
            for (std::size_t i = 0; i < n; ++i) index.add(static_cast<std::uint32_t>(i), names[i]);
            index.finalize();
            const std::vector<std::string> queries = {names[n / 3], "ann loppez" + std::to_string(n % 1000), "kyle ngyuen", "mia", "zhao"};
            measure("trie + bounded edit distance, " + std::to_string(n) + " names", [&](std::size_t i) { auto r = index.query(queries[i % queries.size()], 3); keep(r); }, 50, 100);
            measure("whole-name edit distance against every name, " + std::to_string(n), [&](std::size_t i) {
                const std::string& q = queries[i % queries.size()];
                int best = 1 << 30;
                for (const auto& name : names) best = std::min(best, full_distance(q, name));
                keep(best);
            }, 5, 40);
        }
    }

    if (wanted("lru")) {
        section("a cache of 1,024 entries taking a mix of hits and misses");
        auto run = [&](auto& cache, const char* name) {
            std::mt19937 rng(3);
            std::vector<int> keys(4096);
            for (int& k : keys) k = static_cast<int>(rng() % 1500);
            measure(name, [&](std::size_t i) {
                const int k = keys[i % keys.size()];
                if (!cache.get(k)) cache.put(k, k);
            }, 4096);
        };
        LruCache<int, int> mine(1024);
        NaiveLru naive(1024);
        run(mine, "LruCache: preallocated, open addressing");
        run(naive, "std::list + std::unordered_map");
    }

    if (wanted("queues")) {
        section("one thread handing 5,000,000 items to another");
        constexpr std::uint64_t kCount = 5000000;
        SpscRing<std::uint64_t, 1024> ring;
        NaiveSpsc<std::uint64_t, 1024> naive;
        BoundedQueue<std::uint64_t> locked(1024);
        TryPopAdapter<BoundedQueue<std::uint64_t>> locked_adapter{locked};
        const double a = pipe_throughput(ring, kCount);
        const double b = pipe_throughput(naive, kCount);
        const double c = pipe_throughput(locked_adapter, kCount);
        std::printf("  %-52s %12.1f million items/s\n", "SpscRing: padded, cached indices", a / 1e6);
        std::printf("  %-52s %12.1f million items/s\n", "SPSC ring, indices side by side, no caching", b / 1e6);
        std::printf("  %-52s %12.1f million items/s\n", "BoundedQueue: mutex and condition variables", c / 1e6);
        g_extra["spsc_ring_mitems_per_s"] = a / 1e6;
        g_extra["spsc_naive_mitems_per_s"] = b / 1e6;
        g_extra["bounded_queue_mitems_per_s"] = c / 1e6;
    }

    if (wanted("service")) {
        section("requests per second through the service, by number of workers (each request is 'call mom')");
        Model m3;
        Model::load(model_path, m3, false);
        EngineConfig config;
        config.random_seed = 1;
        Engine prototype(std::move(m3), std::make_shared<FixedClock>(LocalTime{2026, 10, 8, 9, 15, 0}), config);
        prototype.set_contacts({{1, "Mom"}, {2, "Dana"}});
        for (const std::size_t workers : {1UL, 2UL, 4UL, 8UL}) {
            constexpr int kRequests = 20000;
            AssistService service(prototype, {workers, 1024});
            std::atomic<int> done{0};
            const auto t0 = Steady::now();
            for (int i = 0; i < kRequests; ++i) {
                while (service.submit("call mom", [&](AssistService::Outcome&&) { ++done; }) == AssistService::SubmitStatus::Busy) std::this_thread::yield();
            }
            service.shutdown();
            const double secs = std::chrono::duration<double>(Steady::now() - t0).count();
            std::printf("  %-52s %12.0f requests/s\n", (std::to_string(workers) + (workers == 1 ? " worker" : " workers")).c_str(), kRequests / secs);
            g_extra["service_requests_per_s_" + std::to_string(workers) + "_workers"] = kRequests / secs;
        }
    }

    if (wanted("load")) {
        section("getting a model ready (page cache warm)");
        measure("map the file, skip the checksum", [&](std::size_t) { Model m; Model::load(model_path, m, false); keep(m); }, 20, 100);
        measure("map the file and verify the checksum (reads every page)", [&](std::size_t) { Model m; Model::load(model_path, m, true); keep(m); }, 20, 100);
        std::ifstream in(model_path, std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        measure("copy the bytes first, then parse (from_bytes)", [&](std::size_t) {
            Model m;
            Model::from_bytes({reinterpret_cast<const std::byte*>(raw.data()), raw.size()}, m, false);
            keep(m);
        }, 20, 100);
    }

    if (!json_path.empty()) {
        std::ofstream out(json_path);
        out << "{\n  \"rows\": [\n";
        for (std::size_t i = 0; i < g_rows.size(); ++i) {
            out << "    {\"name\": \"" << g_rows[i].name << "\", \"median_ns\": " << g_rows[i].median_ns << ", \"p99_ns\": " << g_rows[i].p99_ns
                << ", \"per_second\": " << g_rows[i].per_second << "}" << (i + 1 < g_rows.size() ? "," : "") << "\n";
        }
        out << "  ],\n  \"extra\": {";
        std::size_t k = 0;
        for (const auto& [name, value] : g_extra) out << (k++ ? ", " : "") << "\"" << name << "\": " << value;
        out << "}\n}\n";
    }
    return 0;
}
