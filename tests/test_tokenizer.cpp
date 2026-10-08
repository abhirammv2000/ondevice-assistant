#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "assist/hash.hpp"
#include "assist/tokenizer.hpp"
#include "doctest.h"

#ifndef ASSIST_GOLDEN_DIR
#define ASSIST_GOLDEN_DIR "tests/golden"
#endif

using namespace assist;

namespace {

std::vector<std::string> tokens_of(std::string_view text) {
    TokenScratch s;
    extract_features(text, s);
    std::vector<std::string> out;
    for (std::uint32_t i = 0; i < s.token_count; ++i) out.emplace_back(s.token(i));
    return out;
}

std::string from_hex(const std::string& hex) {
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

}  // namespace

TEST_CASE("hashing: FNV-1a matches published test vectors") {
    CHECK(fnv1a_append(kFnvOffset, "") == 0xcbf29ce484222325ULL);
    CHECK(fnv1a_append(kFnvOffset, "a") == 0xaf63dc4c8601ec8cULL);
    CHECK(fnv1a_append(kFnvOffset, "foobar") == 0x85944171f73967e8ULL);
}

TEST_CASE("hashing: appending in pieces equals hashing the whole") {
    const auto whole = fnv1a_append(kFnvOffset, "b:set timer");
    const auto parts = fnv1a_append(fnv1a_append(fnv1a_append(kFnvOffset, "b:"), "set"), " timer");
    CHECK(whole == parts);
}

TEST_CASE("hashing: CRC-32 matches the standard check value") {
    constexpr std::string_view s = "123456789";
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    CHECK(crc32({p, s.size()}) == 0xCBF43926U);
    CHECK(crc32({}) == 0U);
}

TEST_CASE("tokenizer: lower-cases, splits on punctuation and keeps apostrophes") {
    CHECK(tokens_of("Hello, World!") == std::vector<std::string>{"hello", "world"});
    CHECK(tokens_of("What's the weather") == std::vector<std::string>{"what's", "the", "weather"});
    CHECK(tokens_of("  a   b\t\nc ") == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("tokenizer: a token of only digits becomes <num>, a mixed token does not") {
    CHECK(tokens_of("timer for 10 minutes") == std::vector<std::string>{"timer", "for", "<num>", "minutes"});
    CHECK(tokens_of("3rd of may") == std::vector<std::string>{"3rd", "of", "may"});
    CHECK(tokens_of("at 7:30") == std::vector<std::string>{"at", "<num>", "<num>"});
}

TEST_CASE("tokenizer: UTF-8 bytes pass through and are not lower-cased") {
    CHECK(tokens_of("Caf\xC3\xA9 au lait") == std::vector<std::string>{"caf\xC3\xA9", "au", "lait"});
}

TEST_CASE("features: numbers share features, so '10 minutes' and '25 minutes' look alike") {
    TokenScratch a, b;
    extract_features("set a timer for 10 minutes", a);
    extract_features("set a timer for 25 minutes", b);
    CHECK(std::equal(a.features, a.features + a.feature_count, b.features, b.features + b.feature_count));
}

TEST_CASE("features: sorted, unique and bounded") {
    TokenScratch s;
    extract_features("the the the the the", s);
    CHECK(std::is_sorted(s.features, s.features + s.feature_count));
    CHECK(std::adjacent_find(s.features, s.features + s.feature_count) == s.features + s.feature_count);
    // 1 distinct unigram, bigrams ^ the, the the, the $
    CHECK(s.feature_count == 4);
}

TEST_CASE("features: no tokens means no features") {
    TokenScratch s;
    extract_features("", s);
    CHECK(s.feature_count == 0);
    extract_features("?!  ,", s);
    CHECK(s.feature_count == 0);
    CHECK(s.token_count == 0);
}

TEST_CASE("features: the scratch is reusable and a second call forgets the first") {
    TokenScratch s;
    extract_features("one two three four five", s);
    extract_features("hi", s);
    CHECK(s.token_count == 1);
    CHECK(s.feature_count == 3);  // u:hi, b:^ hi, b:hi $
}

TEST_CASE("limits: too many tokens stops at the token limit") {
    std::string text;
    for (int i = 0; i < 200; ++i) text += "a ";
    TokenScratch s;
    extract_features(text, s);
    CHECK(s.token_count == kMaxTokens);
    CHECK(s.feature_count <= kMaxFeatures);
}

TEST_CASE("limits: a token that does not fit in the text buffer is dropped with everything after it") {
    CHECK(tokens_of(std::string(600, 'x')).empty());
    std::string text = std::string(500, 'a') + " " + std::string(50, 'b') + " tail";
    const auto got = tokens_of(text);
    REQUIRE(got.size() == 1);
    CHECK(got[0].size() == 500);
}

TEST_CASE("limits: many <num> tokens count 5 bytes each against the text buffer") {
    std::string text;
    for (int i = 0; i < 130; ++i) text += "1 ";
    CHECK(tokens_of(text).size() == kMaxTokens);  // 96 * 5 = 480 bytes, then the token limit
}

TEST_CASE("limits: the worst case never writes past the arrays") {
    std::string text;
    for (int i = 0; i < 96; ++i) text += "w" + std::to_string(i) + " ";  // 96 distinct tokens
    TokenScratch s;
    extract_features(text, s);
    CHECK(s.token_count == 96);
    CHECK(s.feature_count <= kMaxFeatures);
}

TEST_CASE("parity: the C++ features equal the values the Python tools produce") {
    std::ifstream in(std::string(ASSIST_GOLDEN_DIR) + "/features.tsv");
    REQUIRE_MESSAGE(in.good(), "run python tools/make_golden.py first");
    std::string line;
    int checked = 0;
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        REQUIRE(tab != std::string::npos);
        const std::string utterance = from_hex(line.substr(0, tab));
        std::vector<std::uint32_t> want;
        std::stringstream rest(line.substr(tab + 1));
        std::string item;
        while (std::getline(rest, item, ',')) want.push_back(static_cast<std::uint32_t>(std::stoull(item)));

        TokenScratch s;
        extract_features(utterance, s);
        const std::vector<std::uint32_t> got(s.features, s.features + s.feature_count);
        CHECK_MESSAGE(got == want, "case " << checked);
        ++checked;
    }
    CHECK(checked >= 20);
}
