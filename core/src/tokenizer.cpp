#include "assist/tokenizer.hpp"

#include <algorithm>
#include <cstring>

#include "assist/hash.hpp"

namespace assist {
namespace {

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

constexpr bool is_token_byte(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '\'' || u >= 0x80;
}

constexpr char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

constexpr std::string_view kNum = "<num>";

}  // namespace

void extract_features(std::string_view in, TokenScratch& s) noexcept {
    s.token_count = 0;
    s.feature_count = 0;

    std::size_t w = 0;  // write position in s.text
    std::size_t i = 0;
    const std::size_t n = in.size();
    while (s.token_count < kMaxTokens) {
        while (i < n && !is_token_byte(in[i])) ++i;
        if (i >= n) break;
        std::size_t j = i;
        bool all_digits = true;
        while (j < n && is_token_byte(in[j])) {
            all_digits = all_digits && is_digit(in[j]);
            ++j;
        }
        const std::size_t need = all_digits ? kNum.size() : (j - i);
        if (w + need > kMaxUtteranceBytes) break;  // this token and everything after it is dropped

        s.begin[s.token_count] = static_cast<std::uint16_t>(w);
        if (all_digits) {
            std::memcpy(s.text + w, kNum.data(), kNum.size());
            w += kNum.size();
        } else {
            for (std::size_t k = i; k < j; ++k) s.text[w++] = lower(in[k]);
        }
        s.end[s.token_count] = static_cast<std::uint16_t>(w);
        ++s.token_count;
        i = j;
    }
    if (s.token_count == 0) return;

    constexpr std::uint64_t kUnigram = fnv1a_append(kFnvOffset, "u:");
    constexpr std::uint64_t kBigram = fnv1a_append(kFnvOffset, "b:");
    auto bigram = [](std::string_view a, std::string_view b) {
        return fold32(fnv1a_append(fnv1a_append(fnv1a_append(kBigram, a), " "), b));
    };

    std::uint32_t* out = s.features;
    for (std::uint32_t t = 0; t < s.token_count; ++t) *out++ = fold32(fnv1a_append(kUnigram, s.token(t)));
    *out++ = bigram("^", s.token(0));
    for (std::uint32_t t = 0; t + 1 < s.token_count; ++t) *out++ = bigram(s.token(t), s.token(t + 1));
    *out++ = bigram(s.token(s.token_count - 1), "$");

    std::sort(s.features, out);
    const std::uint32_t* const last = std::unique(s.features, out);
    s.feature_count = static_cast<std::uint32_t>(last - s.features);
}

}  // namespace assist
