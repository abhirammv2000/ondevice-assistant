#include "assist/fuzzy.hpp"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>

namespace assist {
namespace {

constexpr bool is_word_byte(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '\'' || u >= 0x80;
}

// lower-case words, split on anything that is not a letter, digit, apostrophe or non-ASCII byte
void split_words(std::string_view text, std::vector<std::string>& out, std::size_t limit) {
    std::size_t i = 0;
    while (i < text.size() && out.size() < limit) {
        while (i < text.size() && !is_word_byte(text[i])) ++i;
        std::string word;
        while (i < text.size() && is_word_byte(text[i])) {
            const char c = text[i++];
            word.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c);
        }
        if (!word.empty()) out.push_back(std::move(word));
    }
}

bool all_ascii_letters(std::string_view w) noexcept {
    return !w.empty() && std::all_of(w.begin(), w.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); });
}

}  // namespace

int bounded_edit_distance(std::string_view a, std::string_view b, int max_distance) noexcept {
    const int la = static_cast<int>(a.size());
    const int lb = static_cast<int>(b.size());
    if (std::abs(la - lb) > max_distance) return max_distance + 1;
    if (la == 0) return std::min(lb, max_distance + 1);
    if (lb == 0) return std::min(la, max_distance + 1);

    constexpr int kStack = 128;
    int stack_rows[3][kStack + 1];
    std::vector<int> heap_rows;
    int* rows[3];
    if (lb <= kStack) {
        for (int r = 0; r < 3; ++r) rows[r] = stack_rows[r];
    } else {
        heap_rows.resize(3 * static_cast<std::size_t>(lb + 1));
        for (int r = 0; r < 3; ++r) rows[r] = heap_rows.data() + r * (lb + 1);
    }
    int* prev2 = rows[0];
    int* prev1 = rows[1];
    int* cur = rows[2];
    for (int j = 0; j <= lb; ++j) prev1[j] = j;
    for (int i = 1; i <= la; ++i) {
        cur[0] = i;
        int row_min = cur[0];
        for (int j = 1; j <= lb; ++j) {
            const int cost = a[static_cast<std::size_t>(i - 1)] == b[static_cast<std::size_t>(j - 1)] ? 0 : 1;
            int v = std::min({prev1[j] + 1, cur[j - 1] + 1, prev1[j - 1] + cost});
            if (i > 1 && j > 1 && a[static_cast<std::size_t>(i - 1)] == b[static_cast<std::size_t>(j - 2)] &&
                a[static_cast<std::size_t>(i - 2)] == b[static_cast<std::size_t>(j - 1)]) {
                v = std::min(v, prev2[j - 2] + 1);
            }
            cur[j] = v;
            row_min = std::min(row_min, v);
        }
        if (row_min > max_distance) return max_distance + 1;  // every path is already too expensive
        int* const oldest = prev2;
        prev2 = prev1;
        prev1 = cur;
        cur = oldest;
    }
    return std::min(prev1[lb], max_distance + 1);
}

std::string soundex(std::string_view word) {
    if (!all_ascii_letters(word)) return {};
    auto code = [](char c) -> char {
        switch (c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c) {
            case 'b': case 'f': case 'p': case 'v': return '1';
            case 'c': case 'g': case 'j': case 'k': case 'q': case 's': case 'x': case 'z': return '2';
            case 'd': case 't': return '3';
            case 'l': return '4';
            case 'm': case 'n': return '5';
            case 'r': return '6';
            default: return '0';  // a e i o u y h w
        }
    };
    std::string out;
    out.push_back(static_cast<char>(word[0] >= 'a' && word[0] <= 'z' ? word[0] - ('a' - 'A') : word[0]));
    char prev = code(word[0]);
    for (std::size_t i = 1; i < word.size() && out.size() < 4; ++i) {
        const char lowered = (word[i] >= 'A' && word[i] <= 'Z') ? static_cast<char>(word[i] + ('a' - 'A')) : word[i];
        if (lowered == 'h' || lowered == 'w') continue;  // these do not separate two letters with the same code
        const char c = code(word[i]);
        if (c != '0' && c != prev) out.push_back(c);
        prev = c;
    }
    while (out.size() < 4) out.push_back('0');
    return out;
}

bool FuzzyIndex::add(std::uint32_t id, std::string_view name) {
    if (finalized_ || entries_.size() >= kMaxEntries) return false;
    std::vector<std::string> words;
    split_words(name, words, 32);
    if (words.empty()) return false;
    entries_.push_back(Entry{id, static_cast<std::uint16_t>(words.size()), std::string(name)});
    pending_words_.push_back(std::move(words));
    return true;
}

void FuzzyIndex::trie_insert(std::string_view word, std::int32_t vocab_id) {
    std::int32_t node = 0;
    for (const char c : word) {
        const auto byte = static_cast<unsigned char>(c);
        std::int32_t child = trie_[static_cast<std::size_t>(node)].first_child;
        while (child != -1 && trie_[static_cast<std::size_t>(child)].byte != byte) child = trie_[static_cast<std::size_t>(child)].next_sibling;
        if (child == -1) {
            TrieNode n;
            n.byte = byte;
            n.next_sibling = trie_[static_cast<std::size_t>(node)].first_child;
            trie_.push_back(n);
            child = static_cast<std::int32_t>(trie_.size() - 1);
            trie_[static_cast<std::size_t>(node)].first_child = child;
        }
        node = child;
    }
    trie_[static_cast<std::size_t>(node)].word = vocab_id;
}

void FuzzyIndex::finalize() {
    if (finalized_) return;
    trie_.assign(1, TrieNode{});
    std::unordered_map<std::string, std::int32_t> ids;
    for (std::size_t e = 0; e < pending_words_.size(); ++e) {
        for (const std::string& w : pending_words_[e]) {
            auto [it, inserted] = ids.try_emplace(w, static_cast<std::int32_t>(vocab_.size()));
            if (inserted) {
                vocab_.push_back(w);
                postings_.emplace_back();
                trie_insert(w, it->second);
            }
            auto& list = postings_[static_cast<std::size_t>(it->second)];
            if (list.empty() || list.back() != e) list.push_back(static_cast<std::uint32_t>(e));  // a word repeated in one name counts once
        }
    }
    for (std::size_t v = 0; v < vocab_.size(); ++v) {
        std::string code = soundex(vocab_[v]);
        if (!code.empty()) phonetic_.emplace_back(std::move(code), static_cast<std::int32_t>(v));
    }
    std::sort(phonetic_.begin(), phonetic_.end());
    pending_words_.clear();
    pending_words_.shrink_to_fit();
    finalized_ = true;
}

std::string_view FuzzyIndex::name_for_id(std::uint32_t id) const noexcept {
    for (const Entry& e : entries_) {
        if (e.id == id) return e.name;
    }
    return {};
}

std::int32_t FuzzyIndex::trie_find(std::string_view word) const noexcept {
    std::int32_t node = 0;
    for (const char c : word) {
        const auto byte = static_cast<unsigned char>(c);
        std::int32_t child = trie_[static_cast<std::size_t>(node)].first_child;
        while (child != -1 && trie_[static_cast<std::size_t>(child)].byte != byte) child = trie_[static_cast<std::size_t>(child)].next_sibling;
        if (child == -1) return -1;
        node = child;
    }
    return node;
}

void FuzzyIndex::collect_prefix(std::int32_t node, std::vector<std::int32_t>& out, std::size_t limit) const {
    std::vector<std::int32_t> stack{node};
    while (!stack.empty() && out.size() < limit) {
        const std::int32_t n = stack.back();
        stack.pop_back();
        const TrieNode& tn = trie_[static_cast<std::size_t>(n)];
        if (tn.word >= 0) out.push_back(tn.word);
        for (std::int32_t c = tn.first_child; c != -1; c = trie_[static_cast<std::size_t>(c)].next_sibling) stack.push_back(c);
    }
}

std::vector<FuzzyMatch> FuzzyIndex::query(std::string_view text, std::size_t k, float min_score) const {
    std::vector<FuzzyMatch> result;
    if (!finalized_ || entries_.empty() || k == 0) return result;
    std::vector<std::string> words;
    split_words(text, words, kMaxQueryWords);
    if (words.empty()) return result;

    using Scores = std::array<float, kMaxQueryWords>;
    std::unordered_map<std::uint32_t, Scores> best;
    auto touch = [&](std::int32_t vocab_id, std::size_t qi, float score) {
        for (const std::uint32_t entry : postings_[static_cast<std::size_t>(vocab_id)]) {
            Scores& s = best.try_emplace(entry, Scores{}).first->second;
            s[qi] = std::max(s[qi], score);
        }
    };

    for (std::size_t qi = 0; qi < words.size(); ++qi) {
        const std::string& w = words[qi];
        // exact, and prefixes: words that start with what was said
        std::int32_t exact_word = -1;
        if (const std::int32_t node = trie_find(w); node >= 0) {
            exact_word = trie_[static_cast<std::size_t>(node)].word;
            if (exact_word >= 0) touch(exact_word, qi, 1.0F);
            if (w.size() >= 2) {
                std::vector<std::int32_t> below;
                collect_prefix(node, below, 64);
                for (const std::int32_t v : below) {
                    if (v == exact_word) continue;
                    const float fill = static_cast<float>(w.size()) / static_cast<float>(vocab_[static_cast<std::size_t>(v)].size());
                    touch(v, qi, 0.8F + 0.15F * fill);
                }
            }
        }
        // misspellings: a few edits away, with fewer allowed for short words
        const int allowed = w.size() <= 3 ? 0 : (w.size() <= 6 ? 1 : 2);
        if (allowed > 0 && w.size() <= 40) {
            for (std::size_t v = 0; v < vocab_.size(); ++v) {
                if (static_cast<std::int32_t>(v) == exact_word) continue;
                const std::string& cand = vocab_[v];
                if (std::abs(static_cast<int>(cand.size()) - static_cast<int>(w.size())) > allowed) continue;
                const int d = bounded_edit_distance(w, cand, allowed);
                if (d <= allowed) touch(static_cast<std::int32_t>(v), qi, 0.9F - 0.12F * static_cast<float>(d));
            }
        }
        // the same sound
        if (w.size() >= 3 && all_ascii_letters(w)) {
            const std::string code = soundex(w);
            auto it = std::lower_bound(phonetic_.begin(), phonetic_.end(), std::make_pair(code, std::int32_t{-1}));
            for (; it != phonetic_.end() && it->first == code; ++it) {
                if (it->second != exact_word) touch(it->second, qi, 0.7F);
            }
        }
    }

    struct Scored {
        std::uint32_t entry;
        float score;
    };
    std::vector<Scored> scored;
    scored.reserve(best.size());
    for (const auto& [entry, per_word] : best) {
        float sum = 0;
        std::size_t matched = 0;
        for (std::size_t i = 0; i < words.size(); ++i) {
            sum += per_word[i];
            matched += per_word[i] > 0 ? 1 : 0;
        }
        const std::size_t extra = entries_[entry].words > matched ? entries_[entry].words - matched : 0;
        const float score = (sum / static_cast<float>(words.size())) * (1.0F - 0.04F * static_cast<float>(std::min<std::size_t>(extra, 5)));
        if (score >= min_score) scored.push_back({entry, score});
    }
    auto better = [&](const Scored& a, const Scored& b) {
        if (a.score != b.score) return a.score > b.score;
        if (entries_[a.entry].name.size() != entries_[b.entry].name.size()) return entries_[a.entry].name.size() < entries_[b.entry].name.size();
        return entries_[a.entry].id < entries_[b.entry].id;
    };
    const std::size_t take = std::min(k, scored.size());
    std::partial_sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(take), scored.end(), better);
    result.reserve(take);
    for (std::size_t i = 0; i < take; ++i) result.push_back({entries_[scored[i].entry].id, scored[i].score});
    return result;
}

}  // namespace assist
