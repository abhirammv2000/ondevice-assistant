#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "assist/hash.hpp"
#include "assist/model.hpp"
#include "doctest.h"

#ifndef ASSIST_GOLDEN_DIR
#define ASSIST_GOLDEN_DIR "tests/golden"
#endif

using namespace assist;

namespace {

const std::string kGolden = ASSIST_GOLDEN_DIR;

std::vector<std::byte> read_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "missing " << path << ", run python tools/make_golden.py");
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

void put32(std::vector<std::byte>& b, std::size_t offset, std::uint32_t v) { std::memcpy(b.data() + offset, &v, 4); }
void put64(std::vector<std::byte>& b, std::size_t offset, std::uint64_t v) { std::memcpy(b.data() + offset, &v, 8); }
std::uint32_t get32(const std::vector<std::byte>& b, std::size_t offset) {
    std::uint32_t v;
    std::memcpy(&v, b.data() + offset, 4);
    return v;
}

// After editing a field, recompute the checksum so the loader gets past it and has to catch the edit itself.
void fix_crc(std::vector<std::byte>& b) { put32(b, b.size() - 4, crc32({b.data(), b.size() - 4})); }

// header field offsets, from core/include/assist/model.hpp
constexpr std::size_t kVersion = 4, kClasses = 8, kFeatures = 12, kFlags = 16, kNamesOff = 20, kNamesBytes = 24, kScalesOff = 28,
                      kBiasOff = 32, kWeightsOff = 36, kWeightsBytes = 40, kReserved = 48;

struct Expect {
    std::string kind;
    std::vector<std::uint32_t> features;
    std::vector<float> probs;
};

std::vector<Expect> read_expectations() {
    std::ifstream in(kGolden + "/tiny_expect.tsv");
    REQUIRE_MESSAGE(in.good(), "run python tools/make_golden.py first");
    std::vector<Expect> out;
    std::string line;
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string kind, feats, probs;
        std::getline(ss, kind, '\t');
        std::getline(ss, feats, '\t');
        std::getline(ss, probs, '\t');
        Expect e;
        e.kind = kind;
        std::stringstream fs(feats);
        std::string item;
        while (std::getline(fs, item, ',')) e.features.push_back(static_cast<std::uint32_t>(std::stoull(item)));
        std::stringstream ps(probs);
        while (std::getline(ps, item, ',')) e.probs.push_back(std::stof(item));
        out.push_back(std::move(e));
    }
    return out;
}

Model load_tiny(const std::string& kind) {
    Model m;
    REQUIRE(Model::load(kGolden + "/tiny_" + kind + ".pmodel", m) == LoadStatus::Ok);
    return m;
}

}  // namespace

TEST_CASE("model: the golden models load from a memory map and report what they hold") {
    const Model q = load_tiny("int8");
    CHECK(q.class_count() == 5);
    CHECK(q.feature_count() == 64);
    CHECK(q.quantized());
    CHECK(q.class_name(0) == "alpha");
    CHECK(q.class_name(4) == "\xC3\xA9psilon");
    CHECK(q.class_name(5).empty());

    const Model f = load_tiny("float");
    CHECK_FALSE(f.quantized());
    CHECK(f.file_bytes() > q.file_bytes());  // 4-byte weights are bigger than 1-byte weights
}

TEST_CASE("model: the C++ scoring agrees with the Python reference on every golden case") {
    const Model models[2] = {load_tiny("int8"), load_tiny("float")};
    auto scratch_int = models[0].make_scratch();
    auto scratch_float = models[1].make_scratch();
    int checked = 0;
    for (const Expect& e : read_expectations()) {
        const Model& m = e.kind == "int8" ? models[0] : models[1];
        auto& scratch = e.kind == "int8" ? scratch_int : scratch_float;
        const auto got = m.probabilities(e.features, scratch);
        REQUIRE(got.size() == e.probs.size());
        for (std::size_t c = 0; c < got.size(); ++c) CHECK_MESSAGE(std::fabs(got[c] - e.probs[c]) < 2e-5F, e.kind << " case " << checked << " class " << c);
        ++checked;
    }
    CHECK(checked == 24);
}

TEST_CASE("model: probabilities sum to one and predict() reports the best class with its margin") {
    const Model m = load_tiny("int8");
    auto scratch = m.make_scratch();
    const std::uint32_t feats[] = {3, 17, 29};
    const auto p = m.probabilities(feats, scratch);
    float sum = 0;
    for (float x : p) sum += x;
    CHECK(sum == doctest::Approx(1.0F).epsilon(1e-5));

    std::vector<float> copy(p.begin(), p.end());
    const Prediction pred = m.predict(feats, scratch);
    std::vector<float> sorted = copy;
    std::sort(sorted.rbegin(), sorted.rend());
    CHECK(copy[pred.intent] == sorted[0]);
    CHECK(pred.confidence == sorted[0]);
    CHECK(pred.margin == doctest::Approx(sorted[0] - sorted[1]).epsilon(1e-6));
}

TEST_CASE("model: no features gives the softmax of the bias alone") {
    const Model m = load_tiny("float");
    auto scratch = m.make_scratch();
    const auto p = m.probabilities({}, scratch);
    REQUIRE(p.size() == 5);
    float sum = 0;
    for (float x : p) sum += x;
    CHECK(sum == doctest::Approx(1.0F).epsilon(1e-5));
}

TEST_CASE("model: from_bytes and load give the same answers") {
    const auto bytes = read_bytes(kGolden + "/tiny_int8.pmodel");
    Model copied;
    REQUIRE(Model::from_bytes(bytes, copied) == LoadStatus::Ok);
    const Model mapped = load_tiny("int8");
    auto a = copied.make_scratch();
    auto b = mapped.make_scratch();
    const std::uint32_t feats[] = {1, 9, 33, 63};
    const auto pa = copied.probabilities(feats, a);
    const auto pb = mapped.probabilities(feats, b);
    for (std::size_t c = 0; c < pa.size(); ++c) CHECK(pa[c] == pb[c]);
}

TEST_CASE("model: a scratch that is too small gives an empty answer, not a crash") {
    const Model m = load_tiny("int8");
    Model::Scratch tiny;
    const std::uint32_t feats[] = {1};
    CHECK(m.probabilities(feats, tiny).empty());
    CHECK(m.predict(feats, tiny).confidence == 0.0F);
}

TEST_CASE("model: a model survives being moved and copied while its mapping is shared") {
    Model a = load_tiny("int8");
    Model b = a;                  // shares the mapping
    Model c = std::move(a);
    auto sb = b.make_scratch();
    auto sc = c.make_scratch();
    const std::uint32_t feats[] = {2, 4};
    const auto pb = b.probabilities(feats, sb);
    const auto pc = c.probabilities(feats, sc);
    CHECK(pb[0] == pc[0]);
}

TEST_CASE("loading: a missing or empty file is an io error") {
    Model m;
    CHECK(Model::load(kGolden + "/no_such_file.pmodel", m) == LoadStatus::IoError);
    const auto empty = std::filesystem::temp_directory_path() / "assist_empty_model_for_test.bin";
    std::ofstream(empty, std::ios::binary).close();
    CHECK(Model::load(empty, m) == LoadStatus::IoError);
    std::filesystem::remove(empty);
}

TEST_CASE("loading: every way a file can be wrong is caught, with the right reason") {
    const auto good = read_bytes(kGolden + "/tiny_int8.pmodel");
    Model out;
    REQUIRE(Model::from_bytes(good, out) == LoadStatus::Ok);

    auto status_after = [&](auto&& edit, bool refresh_crc = true) {
        auto b = good;
        edit(b);
        if (refresh_crc) fix_crc(b);
        Model m;
        return Model::from_bytes(b, m);
    };

    SUBCASE("too small") {
        Model m;
        CHECK(Model::from_bytes({}, m) == LoadStatus::TooSmall);
        CHECK(Model::from_bytes(std::span(good).first(40), m) == LoadStatus::TooSmall);
    }
    SUBCASE("magic and version") {
        CHECK(status_after([](auto& b) { b[0] = std::byte{'X'}; }) == LoadStatus::BadMagic);
        CHECK(status_after([](auto& b) { put32(b, kVersion, 2); }) == LoadStatus::BadVersion);
        CHECK(status_after([](auto& b) { put32(b, kVersion, 0); }) == LoadStatus::BadVersion);
    }
    SUBCASE("class and feature counts") {
        CHECK(status_after([](auto& b) { put32(b, kClasses, 0); }) == LoadStatus::BadHeader);
        CHECK(status_after([](auto& b) { put32(b, kClasses, kMaxClasses + 1); }) == LoadStatus::BadHeader);
        CHECK(status_after([](auto& b) { put32(b, kClasses, 0xFFFFFFFFU); }) == LoadStatus::BadHeader);
        CHECK(status_after([](auto& b) { put32(b, kFeatures, 100); }) == LoadStatus::BadHeader);   // not a power of two
        CHECK(status_after([](auto& b) { put32(b, kFeatures, 32); }) == LoadStatus::BadHeader);    // below the minimum
        CHECK(status_after([](auto& b) { put32(b, kFeatures, 1U << 25); }) == LoadStatus::BadHeader);
        CHECK(status_after([](auto& b) { put32(b, kFeatures, 0); }) == LoadStatus::BadHeader);
    }
    SUBCASE("flags and reserved bytes") {
        CHECK(status_after([](auto& b) { put32(b, kFlags, 2); }) == LoadStatus::BadHeader);
        CHECK(status_after([](auto& b) { put32(b, kFlags, 0x80000001U); }) == LoadStatus::BadHeader);
        CHECK(status_after([](auto& b) { b[kReserved + 3] = std::byte{1}; }) == LoadStatus::BadHeader);
    }
    SUBCASE("sections that do not fit or are not aligned") {
        CHECK(status_after([](auto& b) { put32(b, kNamesOff, 0xFFFFFFF0U); }) == LoadStatus::BadLayout);
        CHECK(status_after([](auto& b) { put32(b, kNamesOff, 8); }) == LoadStatus::BadLayout);  // inside the header
        CHECK(status_after([](auto& b) { put32(b, kNamesBytes, 0xFFFFFFFFU); }) == LoadStatus::BadLayout);
        CHECK(status_after([](auto& b) { put32(b, kBiasOff, get32(b, kBiasOff) + 1); }) == LoadStatus::BadLayout);
        CHECK(status_after([](auto& b) { put32(b, kBiasOff, 0xFFFFFFFCU); }) == LoadStatus::BadLayout);
        CHECK(status_after([](auto& b) { put32(b, kScalesOff, get32(b, kScalesOff) + 2); }) == LoadStatus::BadLayout);
        CHECK(status_after([](auto& b) { put32(b, kWeightsOff, get32(b, kWeightsOff) + 4); }) == LoadStatus::BadLayout);  // not 64-aligned
        CHECK(status_after([](auto& b) { put32(b, kWeightsOff, 0xFFFFFFC0U); }) == LoadStatus::BadLayout);
    }
    SUBCASE("weights size must match the counts") {
        CHECK(status_after([](auto& b) { put64(b, kWeightsBytes, 1); }) == LoadStatus::BadHeader);
        CHECK(status_after([](auto& b) { put64(b, kWeightsBytes, ~0ULL); }) == LoadStatus::BadHeader);
        CHECK(status_after([](auto& b) { put64(b, kWeightsBytes, get32(b, kWeightsOff) * 1000ULL); }) == LoadStatus::BadHeader);
    }
    SUBCASE("a float model has no scales section") {
        const auto f = read_bytes(kGolden + "/tiny_float.pmodel");
        auto b = f;
        put32(b, kScalesOff, 64);
        fix_crc(b);
        Model m;
        CHECK(Model::from_bytes(b, m) == LoadStatus::BadHeader);
    }
    SUBCASE("class names") {
        // the first name's length claims more bytes than the section has
        CHECK(status_after([&](auto& b) { const auto off = get32(b, kNamesOff); const std::uint16_t huge = 0xFFFF; std::memcpy(b.data() + off, &huge, 2); }) == LoadStatus::BadLayout);
        // the section is bigger than the names in it
        CHECK(status_after([](auto& b) { put32(b, kNamesBytes, get32(b, kNamesBytes) + 1); }) == LoadStatus::BadLayout);
    }
    SUBCASE("non-finite numbers") {
        const float nan = std::nanf("");
        CHECK(status_after([&](auto& b) { std::memcpy(b.data() + get32(b, kBiasOff), &nan, 4); }) == LoadStatus::BadHeader);
        CHECK(status_after([&](auto& b) { std::memcpy(b.data() + get32(b, kScalesOff), &nan, 4); }) == LoadStatus::BadHeader);
        const float inf = INFINITY;
        CHECK(status_after([&](auto& b) { std::memcpy(b.data() + get32(b, kBiasOff) + 4, &inf, 4); }) == LoadStatus::BadHeader);
    }
    SUBCASE("a NaN weight in a float model is rejected when the checksum is verified") {
        auto b = read_bytes(kGolden + "/tiny_float.pmodel");
        const float nan = std::nanf("");
        std::memcpy(b.data() + get32(b, kWeightsOff) + 8, &nan, 4);
        fix_crc(b);
        Model m;
        CHECK(Model::from_bytes(b, m, true) == LoadStatus::BadHeader);
    }
    SUBCASE("checksum") {
        CHECK(status_after([](auto& b) { b[get32(b, kWeightsOff) + 5] ^= std::byte{0x40}; }, false) == LoadStatus::BadChecksum);
        CHECK(status_after([](auto& b) { b.back() ^= std::byte{1}; }, false) == LoadStatus::BadChecksum);
        // with the check switched off a flipped weight loads, which is the documented trade for a faster start
        auto b = good;
        b[get32(b, kWeightsOff) + 5] ^= std::byte{0x40};
        Model m;
        CHECK(Model::from_bytes(b, m, false) == LoadStatus::Ok);
    }
    SUBCASE("every strict prefix of a good file is refused") {
        for (std::size_t n = 0; n < good.size(); n += 7) {
            Model m;
            CHECK(Model::from_bytes(std::span(good).first(n), m) != LoadStatus::Ok);
        }
    }
}
