// Turns an utterance into hashed n-gram features without touching the heap.
//
// The whole thing lives in a fixed-size TokenScratch the caller owns, so classifying an utterance allocates
// nothing. That is a deliberate choice for a device with a tight memory budget and a latency target: an allocator
// call has an unpredictable cost, and a stack buffer does not.
//
// The tokenizer is specified in docs/DESIGN.md, and tools/features.py implements the same specification in Python so
// the model is trained on exactly the features the device computes. tests/ checks the two against each other.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace assist {

inline constexpr std::size_t kMaxUtteranceBytes = 512;  // longer input is cut at a token boundary
inline constexpr std::size_t kMaxTokens = 96;
inline constexpr std::size_t kMaxFeatures = 2 * kMaxTokens + 2;  // unigrams, plus bigrams including the two edges

struct TokenScratch {
    char text[kMaxUtteranceBytes];
    std::uint16_t begin[kMaxTokens];
    std::uint16_t end[kMaxTokens];
    std::uint32_t token_count = 0;
    std::uint32_t features[kMaxFeatures];
    std::uint32_t feature_count = 0;

    std::string_view token(std::size_t i) const noexcept { return {text + begin[i], static_cast<std::size_t>(end[i] - begin[i])}; }
    std::span<const std::uint32_t> feature_span() const noexcept { return {features, feature_count}; }
};

// Split, normalise and hash. After the call, scratch.features holds the sorted, de-duplicated 32-bit feature hashes.
//
//  - ASCII letters are lower-cased. Letters, digits, apostrophes and every byte >= 0x80 (so UTF-8 passes through
//    untouched) make up a token. Anything else separates tokens.
//  - A token of only digits becomes "<num>", so "set a timer for 10 minutes" and "...for 25 minutes" share features.
//  - Features are "u:<token>" for each token and "b:<a> <b>" for each neighbouring pair, with "^" and "$" standing
//    for the start and the end of the utterance.
//  - No tokens means no features.
void extract_features(std::string_view utterance, TokenScratch& scratch) noexcept;

}  // namespace assist
