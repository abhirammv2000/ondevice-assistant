// Finding a contact, an app or a song from a name that was heard, not typed.
//
// A speech recogniser spells names the way they sound, so "call jon" may need to reach "John" and "sarah conor"
// "Sarah Connor". The index looks each word of the query up four ways and combines what it finds:
//
//   exact       a trie lookup, so the cost depends on the length of the word and not on how many names there are
//   prefix      the words below that trie node, for "chris" meaning "Christopher"
//   misspelling a bounded Damerau-Levenshtein distance over the distinct words, which counts a swapped pair of
//               letters as one edit and gives up early once the distance passes the allowed maximum
//   phonetic    American Soundex codes, for "smyth" and "smith"
//
// A name scores by how well the words of the query are covered, with a small penalty for words in the name that the
// query did not mention, so "john" prefers "John" to "John Quincy Adams". Names are matched case-insensitively
// for ASCII letters only: other bytes must match exactly.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace assist {

struct FuzzyMatch {
    std::uint32_t id;
    float score;  // 0 to 1, best first
};

// The edit distance between two strings, counting insert, delete, replace and swapping two neighbours as one edit.
// Returns max_distance + 1 when the strings are further apart than that, without finishing the calculation.
int bounded_edit_distance(std::string_view a, std::string_view b, int max_distance) noexcept;

// American Soundex of a word made only of ASCII letters, such as "S530" for "smith", or an empty string otherwise.
std::string soundex(std::string_view word);

class FuzzyIndex {
public:
    static constexpr std::size_t kMaxEntries = 100000;
    static constexpr std::size_t kMaxQueryWords = 8;

    // Add a name. Empty names and names added after finalize() are ignored. Returns whether it was added.
    bool add(std::uint32_t id, std::string_view name);
    // Build the trie and the phonetic table. Call once, after the last add().
    void finalize();

    std::size_t size() const noexcept { return entries_.size(); }
    // The name an id was added with, or an empty view for an id that is not in the index.
    std::string_view name_for_id(std::uint32_t id) const noexcept;
    bool finalized() const noexcept { return finalized_; }

    // Up to k names that match the text, best first, each scoring at least min_score. Allocates its result.
    std::vector<FuzzyMatch> query(std::string_view text, std::size_t k = 3, float min_score = 0.5F) const;

private:
    struct Entry {
        std::uint32_t id;
        std::uint16_t words;
        std::string name;
    };
    struct TrieNode {
        std::int32_t first_child = -1;
        std::int32_t next_sibling = -1;
        std::int32_t word = -1;  // index into vocab_ when a word ends here
        unsigned char byte = 0;
    };

    std::int32_t trie_find(std::string_view word) const noexcept;
    void trie_insert(std::string_view word, std::int32_t vocab_id);
    void collect_prefix(std::int32_t node, std::vector<std::int32_t>& out, std::size_t limit) const;

    std::vector<Entry> entries_;
    std::vector<std::string> vocab_;                    // each distinct word
    std::vector<std::vector<std::uint32_t>> postings_;  // for each word, the entries it appears in
    std::vector<TrieNode> trie_;
    std::vector<std::pair<std::string, std::int32_t>> phonetic_;  // soundex code, word, sorted by code
    std::vector<std::vector<std::string>> pending_words_;         // words of each entry, until finalize()
    bool finalized_ = false;
};

}  // namespace assist
