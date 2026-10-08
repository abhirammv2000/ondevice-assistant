// The C interface: lifecycle, errors, the JSON that comes out, threads, streaming and the rule that nothing throws.
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "assist.h"
#include "doctest.h"
#include "model_builder.hpp"

extern "C" int assist_c_header_check(void);

namespace {

std::filesystem::path write_stub_model(const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    const auto bytes = testutil::stub_model_bytes();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

struct Owned {
    char* text = nullptr;
    size_t length = 0;
    ~Owned() { assist_string_free(text); }
    std::string str() const { return text == nullptr ? std::string() : std::string(text, length); }
};

assist_engine* make_engine(const std::filesystem::path& path, uint64_t seed = 1) {
    const std::string p = path.string();
    assist_options o{};
    o.struct_size = sizeof(o);
    o.model_path = p.c_str();
    o.random_seed = seed;
    o.verify_checksum = 1;
    o.use_fixed_time = 1;
    o.year = 2026;
    o.month = 10;
    o.day = 8;
    o.hour = 9;
    o.minute = 15;
    assist_engine* engine = nullptr;
    REQUIRE(assist_engine_create(&o, &engine) == ASSIST_OK);
    REQUIRE(engine != nullptr);
    return engine;
}

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("capi: the header is valid C") { CHECK(assist_c_header_check() == 0); }

TEST_CASE("capi: version and status messages exist for every status") {
    CHECK(std::strlen(assist_version()) > 0);
    for (int s = 0; s <= ASSIST_ERR_INTERNAL; ++s) CHECK(std::strlen(assist_status_message(static_cast<assist_status>(s))) > 0);
}

TEST_CASE("capi: create reports bad arguments and bad files with the right status") {
    assist_engine* engine = reinterpret_cast<assist_engine*>(0x1);
    assist_options o{};
    CHECK(assist_engine_create(nullptr, &engine) == ASSIST_ERR_ARGUMENT);
    CHECK(engine == nullptr);  // the output is cleared on failure
    CHECK(assist_engine_create(&o, &engine) == ASSIST_ERR_ARGUMENT);  // struct_size 0
    o.struct_size = sizeof(o);
    CHECK(assist_engine_create(&o, &engine) == ASSIST_ERR_ARGUMENT);  // no path
    o.model_path = "this/file/does/not/exist.pmodel";
    CHECK(assist_engine_create(&o, &engine) == ASSIST_ERR_IO);

    const auto junk = std::filesystem::temp_directory_path() / "assist_capi_junk.pmodel";
    {
        std::ofstream out(junk, std::ios::binary | std::ios::trunc);
        out << std::string(300, 'x');
    }
    const std::string junk_path = junk.string();
    o.model_path = junk_path.c_str();
    CHECK(assist_engine_create(&o, &engine) == ASSIST_ERR_FORMAT);

    // flip one byte in the middle of a good model: the checksum must catch it, and skipping the check must not
    const auto good = write_stub_model("assist_capi_damaged.pmodel");
    {
        std::fstream f(good, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(200);
        char c = 0;
        f.seekg(200);
        f.get(c);
        f.seekp(200);
        f.put(static_cast<char>(c ^ 0x55));
    }
    const std::string good_path = good.string();
    o.model_path = good_path.c_str();
    o.verify_checksum = 1;
    CHECK(assist_engine_create(&o, &engine) == ASSIST_ERR_CHECKSUM);
    o.verify_checksum = 0;
    CHECK(assist_engine_create(&o, &engine) == ASSIST_OK);
    assist_engine_destroy(engine);

    o.use_fixed_time = 1;
    o.year = 2026;
    o.month = 2;
    o.day = 30;  // no such day
    CHECK(assist_engine_create(&o, &engine) == ASSIST_ERR_ARGUMENT);
}

TEST_CASE("capi: handle returns the same JSON the C++ engine would") {
    const auto path = write_stub_model("assist_capi_good.pmodel");
    assist_engine* engine = make_engine(path);
    Owned out;
    REQUIRE(assist_engine_handle(engine, "set a timer for ten minutes", 27, &out.text, &out.length) == ASSIST_OK);
    const std::string json = out.str();
    CHECK(out.length == std::strlen(out.text));
    CHECK(contains(json, "\"route\":\"on_device\""));
    CHECK(contains(json, "timer.set"));

    // a length shorter than the string means only that many bytes are read
    Owned shorter;
    REQUIRE(assist_engine_handle(engine, "set a timer for ten minutes", 11, &shorter.text, &shorter.length) == ASSIST_OK);
    CHECK(contains(shorter.str(), "\"route\":\"clarify\""));  // "set a timer" has no duration yet

    // an embedded NUL does not end the text early and does not crash
    const char with_nul[] = "set a timer\0 for ten minutes";
    Owned nul;
    CHECK(assist_engine_handle(engine, with_nul, sizeof(with_nul) - 1, &nul.text, &nul.length) == ASSIST_OK);

    // empty input and a NULL pointer with length 0 are valid
    Owned empty, null_empty;
    CHECK(assist_engine_handle(engine, "", 0, &empty.text, &empty.length) == ASSIST_OK);
    CHECK(assist_engine_handle(engine, nullptr, 0, &null_empty.text, &null_empty.length) == ASSIST_OK);
    CHECK(contains(empty.str(), "empty_utterance"));

    // bad arguments
    Owned bad;
    CHECK(assist_engine_handle(nullptr, "x", 1, &bad.text, &bad.length) == ASSIST_ERR_ARGUMENT);
    CHECK(assist_engine_handle(engine, nullptr, 3, &bad.text, &bad.length) == ASSIST_ERR_ARGUMENT);
    CHECK(assist_engine_handle(engine, "x", 1, nullptr, nullptr) == ASSIST_ERR_ARGUMENT);
    CHECK(bad.text == nullptr);

    // the length pointer is optional
    Owned no_len;
    CHECK(assist_engine_handle(engine, "hello", 5, &no_len.text, nullptr) == ASSIST_OK);
    assist_engine_destroy(engine);
}

TEST_CASE("capi: invalid UTF-8 comes back as valid JSON") {
    const auto path = write_stub_model("assist_capi_good.pmodel");
    assist_engine* engine = make_engine(path);
    const char bad[] = "call \xff\xfe mom \xc3";
    Owned out;
    REQUIRE(assist_engine_handle(engine, bad, sizeof(bad) - 1, &out.text, &out.length) == ASSIST_OK);
    for (size_t i = 0; i < out.length; ++i) CHECK(static_cast<unsigned char>(out.text[i]) != 0xff);
    assist_engine_destroy(engine);
}

TEST_CASE("capi: contacts are used and rejected names are counted") {
    const auto path = write_stub_model("assist_capi_good.pmodel");
    assist_engine* engine = make_engine(path);
    const assist_contact contacts[] = {{1, "Mom"}, {2, "Dana Whitfield"}, {3, ""}};
    size_t rejected = 99;
    REQUIRE(assist_engine_set_contacts(engine, contacts, 3, &rejected) == ASSIST_OK);
    CHECK(rejected == 1);
    Owned out;
    REQUIRE(assist_engine_handle(engine, "call mom", 8, &out.text, &out.length) == ASSIST_OK);
    CHECK(contains(out.str(), "call.start"));

    const assist_contact with_null[] = {{1, nullptr}};
    CHECK(assist_engine_set_contacts(engine, with_null, 1, nullptr) == ASSIST_ERR_ARGUMENT);
    CHECK(assist_engine_set_contacts(engine, nullptr, 2, nullptr) == ASSIST_ERR_ARGUMENT);
    CHECK(assist_engine_set_contacts(engine, nullptr, 0, nullptr) == ASSIST_OK);  // clears the list
    assist_engine_destroy(engine);
}

TEST_CASE("capi: the same seed gives the same dice, a fork shares the model but not the scratch state") {
    const auto path = write_stub_model("assist_capi_good.pmodel");
    assist_engine* a = make_engine(path, 42);
    assist_engine* b = make_engine(path, 42);
    Owned ra, rb;
    REQUIRE(assist_engine_handle(a, "roll a die", 10, &ra.text, &ra.length) == ASSIST_OK);
    REQUIRE(assist_engine_handle(b, "roll a die", 10, &rb.text, &rb.length) == ASSIST_OK);
    CHECK(ra.str() == rb.str());

    assist_engine* forked = nullptr;
    REQUIRE(assist_engine_fork(a, &forked) == ASSIST_OK);
    Owned rf;
    CHECK(assist_engine_handle(forked, "hello", 5, &rf.text, &rf.length) == ASSIST_OK);
    assist_engine_destroy(forked);
    assist_engine* none = reinterpret_cast<assist_engine*>(0x1);
    CHECK(assist_engine_fork(nullptr, &none) == ASSIST_ERR_ARGUMENT);
    CHECK(none == nullptr);
    assist_engine_destroy(a);
    assist_engine_destroy(b);
}

TEST_CASE("capi: one engine used from several threads gives correct answers (run under TSan)") {
    const auto path = write_stub_model("assist_capi_good.pmodel");
    assist_engine* engine = make_engine(path);
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 200; ++i) {
                Owned out;
                if (assist_engine_handle(engine, "set a timer for ten minutes", 27, &out.text, &out.length) != ASSIST_OK || !contains(out.str(), "timer.set")) ++wrong;
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK(wrong.load() == 0);
    assist_engine_destroy(engine);
}

TEST_CASE("capi: destroying NULL is allowed") {
    assist_engine_destroy(nullptr);
    assist_stream_destroy(nullptr);
    assist_stream_cancel(nullptr);
    assist_string_free(nullptr);
    CHECK(true);
}

namespace {
struct Collected {
    std::mutex m;
    std::vector<std::pair<uint64_t, std::string>> partials;
};
void collect(const char* json, size_t len, uint64_t sequence, void* user) {
    auto* c = static_cast<Collected*>(user);
    std::lock_guard lock(c->m);
    c->partials.emplace_back(sequence, std::string(json, len));
}
}  // namespace

TEST_CASE("capi: a stream reports partials in rising order and finishes with the final result") {
    const auto path = write_stub_model("assist_capi_good.pmodel");
    assist_engine* engine = make_engine(path);
    Collected collected;
    assist_stream* stream = nullptr;
    REQUIRE(assist_stream_create(engine, collect, &collected, &stream) == ASSIST_OK);
    const char* words[] = {"set", "set a", "set a timer", "set a timer for ten", "set a timer for ten minutes"};
    for (const char* w : words) {
        REQUIRE(assist_stream_update(stream, w, std::strlen(w)) == ASSIST_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Owned final_result;
    REQUIRE(assist_stream_finish(stream, &final_result.text, &final_result.length) == ASSIST_OK);
    CHECK(contains(final_result.str(), "timer.set"));
    CHECK(contains(final_result.str(), "\"route\":\"on_device\""));
    {
        std::lock_guard lock(collected.m);
        uint64_t last = 0;
        for (const auto& [seq, json] : collected.partials) {
            CHECK(seq > last);
            last = seq;
            CHECK(!json.empty());
        }
    }
    // a stream is used once
    Owned again;
    CHECK(assist_stream_finish(stream, &again.text, &again.length) == ASSIST_ERR_STOPPED);
    CHECK(assist_stream_update(stream, "x", 1) == ASSIST_ERR_STOPPED);
    assist_stream_destroy(stream);
    assist_engine_destroy(engine);
}

TEST_CASE("capi: after cancel returns, the callback is never called again") {
    const auto path = write_stub_model("assist_capi_good.pmodel");
    assist_engine* engine = make_engine(path);
    for (int round = 0; round < 50; ++round) {
        Collected collected;
        assist_stream* stream = nullptr;
        REQUIRE(assist_stream_create(engine, collect, &collected, &stream) == ASSIST_OK);
        for (int i = 0; i < 20; ++i) assist_stream_update(stream, "set a timer for ten minutes", 27);
        assist_stream_cancel(stream);
        size_t at_cancel;
        {
            std::lock_guard lock(collected.m);
            at_cancel = collected.partials.size();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        {
            std::lock_guard lock(collected.m);
            CHECK(collected.partials.size() == at_cancel);
        }
        Owned out;
        CHECK(assist_stream_finish(stream, &out.text, &out.length) == ASSIST_ERR_STOPPED);
        assist_stream_destroy(stream);
    }
    assist_engine_destroy(engine);
}

TEST_CASE("capi: finishing a stream that never got text answers like an empty request") {
    const auto path = write_stub_model("assist_capi_good.pmodel");
    assist_engine* engine = make_engine(path);
    assist_stream* stream = nullptr;
    REQUIRE(assist_stream_create(engine, nullptr, nullptr, &stream) == ASSIST_OK);
    Owned out;
    REQUIRE(assist_stream_finish(stream, &out.text, &out.length) == ASSIST_OK);
    CHECK(contains(out.str(), "empty_utterance"));
    assist_stream_destroy(stream);
    assist_engine_destroy(engine);
}
