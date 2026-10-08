#include <cctype>
#include <cstdint>
#include <string>

#include "assist/json.hpp"
#include "doctest.h"

using namespace assist;

namespace {

std::string json_of(std::string_view s) {
    std::string out;
    append_json_string(out, s);
    return out;
}

// An independent check, written a different way from the writer: decode each sequence to a code point and test
// the rules from RFC 3629 on the value, instead of testing byte ranges.
bool is_well_formed_utf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        std::size_t n = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        std::uint32_t cp = n == 1 ? b : b & (0xFFU >> (n + 1));
        for (std::size_t k = 1; k < n; ++k) {
            const auto c = static_cast<unsigned char>(s[i + k]);
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3FU);
        }
        const std::uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
        if (n > 1 && cp < minimum[n]) return false;          // overlong
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;      // surrogate
        if (cp > 0x10FFFF) return false;
        i += n;
    }
    return true;
}

// the body of a JSON string is valid if it has no raw control characters and every backslash starts a legal escape
bool is_valid_json_string(const std::string& q) {
    if (q.size() < 2 || q.front() != '"' || q.back() != '"') return false;
    for (std::size_t i = 1; i + 1 < q.size(); ++i) {
        const auto c = static_cast<unsigned char>(q[i]);
        if (c < 0x20) return false;
        if (c == '"') return false;
        if (c == '\\') {
            ++i;
            if (i + 1 >= q.size()) return false;
            const char e = q[i];
            if (std::string_view("\"\\/bfnrt").find(e) != std::string_view::npos) continue;
            if (e != 'u' || i + 4 >= q.size()) return false;
            for (int k = 1; k <= 4; ++k) {
                if (!std::isxdigit(static_cast<unsigned char>(q[i + static_cast<std::size_t>(k)]))) return false;
            }
            i += 4;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("json: plain text and the characters that must be escaped") {
    CHECK(json_of("hello") == "\"hello\"");
    CHECK(json_of("") == "\"\"");
    CHECK(json_of("say \"hi\"") == "\"say \\\"hi\\\"\"");
    CHECK(json_of("a\\b") == "\"a\\\\b\"");
    CHECK(json_of("line\nbreak\ttab\r") == "\"line\\nbreak\\ttab\\r\"");
    CHECK(json_of(std::string("\x01\x1f", 2)) == "\"\\u0001\\u001f\"");
    CHECK(json_of(std::string("a\0b", 3)) == "\"a\\u0000b\"");
    CHECK(json_of("\x7f") == "\"\x7f\"");
}

TEST_CASE("json: valid UTF-8 passes through unchanged") {
    CHECK(json_of("caf\xC3\xA9") == "\"caf\xC3\xA9\"");                    // two bytes
    CHECK(json_of("\xE2\x82\xAC 5") == "\"\xE2\x82\xAC 5\"");              // three bytes, the euro sign
    CHECK(json_of("\xF0\x9F\x98\x80") == "\"\xF0\x9F\x98\x80\"");           // four bytes, an emoji
    CHECK(json_of("\xF4\x8F\xBF\xBF") == "\"\xF4\x8F\xBF\xBF\"");           // the last code point, U+10FFFF
    CHECK(json_of("\xED\x9F\xBF") == "\"\xED\x9F\xBF\"");                    // just below the surrogates
}

TEST_CASE("json: every kind of malformed UTF-8 is replaced, one byte at a time") {
    CHECK(json_of("\x80") == "\"\\ufffd\"");                       // a lone continuation byte
    CHECK(json_of("\xFF") == "\"\\ufffd\"");
    CHECK(json_of("\xC0\xAF") == "\"\\ufffd\\ufffd\"");           // overlong slash
    CHECK(json_of("\xC1\xBF") == "\"\\ufffd\\ufffd\"");
    CHECK(json_of("\xE0\x80\x80") == "\"\\ufffd\\ufffd\\ufffd\"");  // overlong
    CHECK(json_of("\xED\xA0\x80") == "\"\\ufffd\\ufffd\\ufffd\"");  // a surrogate
    CHECK(json_of("\xF4\x90\x80\x80") == "\"\\ufffd\\ufffd\\ufffd\\ufffd\"");  // above U+10FFFF
    CHECK(json_of("\xF8\x88\x80\x80\x80") == "\"\\ufffd\\ufffd\\ufffd\\ufffd\\ufffd\"");
    CHECK(json_of("\xE2\x82") == "\"\\ufffd\\ufffd\"");           // cut short
    CHECK(json_of("ok\xC3") == "\"ok\\ufffd\"");
    CHECK(json_of("\xC3(") == "\"\\ufffd(\"");                     // a lead byte followed by ASCII
}

TEST_CASE("json: whatever the bytes, the output is a valid JSON string of valid UTF-8") {
    std::uint32_t state = 2026;
    auto next = [&] {
        state = state * 1664525U + 1013904223U;
        return state >> 8;
    };
    for (int i = 0; i < 20000; ++i) {
        std::string s;
        for (unsigned n = next() % 40; n > 0; --n) s.push_back(static_cast<char>(next() % 256));
        const std::string q = json_of(s);
        REQUIRE(is_valid_json_string(q));
        REQUIRE(is_well_formed_utf8(q));
    }
}

TEST_CASE("json: an exhaustive check of every two-byte and three-byte prefix shape") {
    for (unsigned a = 0x80; a <= 0xFF; ++a) {
        for (unsigned b = 0x00; b <= 0xFF; b += 1) {
            const std::string s{static_cast<char>(a), static_cast<char>(b)};
            REQUIRE(is_well_formed_utf8(json_of(s)));
        }
    }
}

TEST_CASE("json: a Result becomes an object with every field, and user text inside it is escaped") {
    Result r;
    r.route = Route::OnDevice;
    r.intent = "timer";
    r.confidence = 0.9877F;
    r.margin = 0.5F;
    r.action = "timer.set";
    r.reply = "Timer set for \"10 minutes\".";
    r.slots = {{"seconds", "600"}, {"note", "a\nb"}};
    r.reason = "handled";
    CHECK(to_json(r) ==
          "{\"route\":\"on_device\",\"intent\":\"timer\",\"confidence\":0.9877,\"margin\":0.5000,\"action\":\"timer.set\","
          "\"reply\":\"Timer set for \\\"10 minutes\\\".\",\"forward_text\":\"\",\"reason\":\"handled\","
          "\"slots\":{\"seconds\":\"600\",\"note\":\"a\\nb\"}}");

    Result empty;
    CHECK(to_json(empty).find("\"slots\":{}") != std::string::npos);
    CHECK(to_json(empty).find("\"route\":\"escalate\"") != std::string::npos);
}
