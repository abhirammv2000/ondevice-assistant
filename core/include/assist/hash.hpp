// FNV-1a hashing and CRC-32, both small enough to read in a minute and both defined by a public spec,
// so the Python tools can produce the same values and the tests can compare them.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace assist {

inline constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

// Continue a 64-bit FNV-1a hash with more bytes. Hashing "ab" then "cd" gives the same value as hashing "abcd",
// which is what lets the tokenizer hash a bigram without building a string for it.
constexpr std::uint64_t fnv1a_append(std::uint64_t h, std::string_view bytes) noexcept {
    for (char c : bytes) {
        h ^= static_cast<unsigned char>(c);
        h *= kFnvPrime;
    }
    return h;
}

// Fold 64 bits into 32 so the low bits, which pick the bucket, depend on all of them.
constexpr std::uint32_t fold32(std::uint64_t h) noexcept {
    return static_cast<std::uint32_t>(h ^ (h >> 32));
}

namespace detail {
constexpr std::array<std::uint32_t, 256> make_crc_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    return table;
}
inline constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();
}  // namespace detail

// CRC-32 as used by zlib, gzip and PNG (polynomial 0xEDB88320, reflected). Python's zlib.crc32 gives the same value.
constexpr std::uint32_t crc32(std::span<const std::byte> data) noexcept {
    std::uint32_t c = 0xFFFFFFFFU;
    for (std::byte b : data) c = detail::kCrcTable[(c ^ static_cast<std::uint32_t>(b)) & 0xFFU] ^ (c >> 8);
    return c ^ 0xFFFFFFFFU;
}

}  // namespace assist
