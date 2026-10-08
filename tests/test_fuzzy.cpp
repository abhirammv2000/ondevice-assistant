#include <algorithm>
#include <string>
#include <vector>

#include "assist/fuzzy.hpp"
#include "doctest.h"

using namespace assist;

namespace {

// the straightforward full-table version, to check the banded early-exit one against
int reference_distance(const std::string& a, const std::string& b) {
    const std::size_t n = a.size(), m = b.size();
    std::vector<std::vector<int>> d(n + 1, std::vector<int>(m + 1, 0));
    for (std::size_t i = 0; i <= n; ++i) d[i][0] = static_cast<int>(i);
    for (std::size_t j = 0; j <= m; ++j) d[0][j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= n; ++i) {
        for (std::size_t j = 1; j <= m; ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
        }
    }
    return d[n][m];
}

FuzzyIndex contacts() {
    FuzzyIndex idx;
    idx.add(1, "Mom");
    idx.add(2, "Dad");
    idx.add(3, "John Smith");
    idx.add(4, "Jon Snow");
    idx.add(5, "Sarah Connor");
    idx.add(6, "Sara Conner");
    idx.add(7, "Christopher Walken");
    idx.add(8, "John Quincy Adams");
    idx.add(9, "Katherine Johnson");
    idx.add(10, "Dana Smythe");
    idx.add(11, "Jos\xC3\xA9 \xC3\x81lvarez");
    idx.add(12, "O'Brien Patrick");
    idx.finalize();
    return idx;
}

std::uint32_t top(const FuzzyIndex& idx, std::string_view q) {
    const auto r = idx.query(q, 1);
    return r.empty() ? 0 : r[0].id;
}

}  // namespace

TEST_CASE("edit distance: known values, including a swap of neighbours as one edit") {
    CHECK(bounded_edit_distance("kitten", "sitting", 5) == 3);
    CHECK(bounded_edit_distance("abcd", "acbd", 3) == 1);
    CHECK(bounded_edit_distance("john", "jhon", 3) == 1);
    CHECK(bounded_edit_distance("same", "same", 3) == 0);
    CHECK(bounded_edit_distance("", "abc", 5) == 3);
    CHECK(bounded_edit_distance("abc", "", 5) == 3);
    CHECK(bounded_edit_distance("", "", 2) == 0);
}

TEST_CASE("edit distance: gives up once the answer is above the maximum") {
    CHECK(bounded_edit_distance("aaaa", "bbbb", 2) == 3);
    CHECK(bounded_edit_distance("short", "a much longer string", 2) == 3);
    CHECK(bounded_edit_distance("abc", "", 2) == 3);
}

TEST_CASE("edit distance: very long strings work too") {
    const std::string a(300, 'a');
    std::string b = a;
    b[150] = 'b';
    CHECK(bounded_edit_distance(a, b, 3) == 1);
    CHECK(bounded_edit_distance(a, a + "xx", 3) == 2);
}

TEST_CASE("edit distance: equals the full-table answer, capped at max + 1, on many random strings") {
    std::uint32_t state = 31337;
    auto next = [&] {
        state = state * 1664525U + 1013904223U;
        return state >> 8;
    };
    for (int i = 0; i < 20000; ++i) {
        std::string a, b;
        for (unsigned n = next() % 9; n > 0; --n) a.push_back(static_cast<char>('a' + next() % 3));
        for (unsigned n = next() % 9; n > 0; --n) b.push_back(static_cast<char>('a' + next() % 3));
        const int full = reference_distance(a, b);
        for (const int max : {0, 1, 2, 3, 10}) {
            REQUIRE_MESSAGE(bounded_edit_distance(a, b, max) == std::min(full, max + 1), a << " " << b << " max " << max);
        }
    }
}

TEST_CASE("soundex: the standard examples") {
    CHECK(soundex("smith") == "S530");
    CHECK(soundex("Smyth") == "S530");
    CHECK(soundex("Robert") == "R163");
    CHECK(soundex("Rupert") == "R163");
    CHECK(soundex("Rubin") == "R150");
    CHECK(soundex("Ashcraft") == "A261");
    CHECK(soundex("Tymczak") == "T522");
    CHECK(soundex("Pfister") == "P236");
    CHECK(soundex("Honeyman") == "H555");
    CHECK(soundex("a") == "A000");
}

TEST_CASE("soundex: anything that is not only ASCII letters has no code") {
    CHECK(soundex("").empty());
    CHECK(soundex("123").empty());
    CHECK(soundex("ab1").empty());
    CHECK(soundex("o'brien").empty());
    CHECK(soundex("caf\xC3\xA9").empty());
}

TEST_CASE("index: an exact name, in any case, wins") {
    const auto idx = contacts();
    CHECK(top(idx, "mom") == 1);
    CHECK(top(idx, "MOM") == 1);
    CHECK(top(idx, "John Smith") == 3);
    CHECK(top(idx, "sarah connor") == 5);
    CHECK(idx.query("john smith", 1)[0].score == doctest::Approx(1.0F));
}

TEST_CASE("index: the shorter, closer name beats one with extra words") {
    const auto idx = contacts();
    const auto r = idx.query("john", 3);
    REQUIRE(r.size() >= 2);
    CHECK(r[0].id == 3);  // John Smith, one extra word
    CHECK(r[1].id == 8);  // John Quincy Adams, two extra words
    CHECK(r[0].score > r[1].score);
}

TEST_CASE("index: prefixes, misspellings and sound-alikes") {
    const auto idx = contacts();
    CHECK(top(idx, "chris") == 7);
    CHECK(top(idx, "christopher") == 7);
    CHECK(top(idx, "jhon smith") == 3);        // two letters swapped
    CHECK(top(idx, "sarah conor") == 5);       // one letter missing
    CHECK(top(idx, "katharine johnson") == 9); // two small slips
    CHECK(top(idx, "dana smith") != 0);
    const auto smyth = idx.query("smyth", 5);
    bool found_smith = false;
    for (const auto& m : smyth) found_smith = found_smith || m.id == 3;
    CHECK(found_smith);                        // "smyth" sounds like "smith"
}

TEST_CASE("index: the two close names are both returned, best first") {
    const auto idx = contacts();
    const auto r = idx.query("sarah conner", 3);
    REQUIRE(r.size() >= 2);
    const bool has5 = r[0].id == 5 || r[1].id == 5;
    const bool has6 = r[0].id == 6 || r[1].id == 6;
    CHECK(has5);
    CHECK(has6);
    CHECK(r[0].score >= r[1].score);
}

TEST_CASE("index: non-ASCII and apostrophes") {
    const auto idx = contacts();
    CHECK(top(idx, "jos\xC3\xA9") == 11);
    CHECK(top(idx, "o'brien") == 12);
    CHECK(top(idx, "patrick") == 12);
}

TEST_CASE("index: no match, empty input and limits") {
    const auto idx = contacts();
    CHECK(idx.query("zzzzzz", 3).empty());
    CHECK(idx.query("", 3).empty());
    CHECK(idx.query("   ", 3).empty());
    CHECK(idx.query("john", 0).empty());
    CHECK(idx.query("john", 1).size() == 1);
    CHECK(idx.query("john", 100).size() <= 12);
    // a high bar excludes weak matches
    CHECK(idx.query("jhon", 3, 0.99F).empty());
    CHECK_FALSE(idx.query("jhon", 3, 0.5F).empty());
}

TEST_CASE("index: more query words than the limit are ignored, not a crash") {
    const auto idx = contacts();
    CHECK_NOTHROW(idx.query("a b c d e f g h i j k l m n o p john smith", 3));
}

TEST_CASE("index: adding after finalize does nothing, and an unfinalized index answers nothing") {
    FuzzyIndex idx;
    CHECK(idx.add(1, "Alice"));
    CHECK(idx.query("alice", 1).empty());  // not finalized yet
    idx.finalize();
    CHECK_FALSE(idx.add(2, "Bob"));
    CHECK(idx.size() == 1);
    CHECK(top(idx, "alice") == 1);
    CHECK_FALSE(idx.add(3, ""));
}

TEST_CASE("index: every name finds itself first, among a thousand generated names") {
    const char* first[] = {"ann", "ben", "cara", "dan", "eva", "finn", "gus", "hana", "ivan", "jade"};
    const char* last[] = {"lopez", "nguyen", "oneil", "patel", "quinn", "rossi", "singh", "tran", "ueda", "vega"};
    FuzzyIndex idx;
    std::vector<std::string> names;
    std::uint32_t id = 1;
    for (const char* f : first) {
        for (const char* l : last) {
            for (int n = 1; n <= 10; ++n) {
                names.push_back(std::string(f) + " " + l + " " + std::to_string(n * 7));
                idx.add(id++, names.back());
            }
        }
    }
    idx.finalize();
    REQUIRE(idx.size() == 1000);
    for (std::uint32_t i = 0; i < names.size(); i += 13) {
        const auto r = idx.query(names[i], 1);
        REQUIRE_MESSAGE(!r.empty(), names[i]);
        CHECK_MESSAGE(r[0].id == i + 1, names[i]);
    }
}
