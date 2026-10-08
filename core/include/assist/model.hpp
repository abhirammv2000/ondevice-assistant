// The intent classifier: a linear model over hashed n-gram features, stored in one file and used straight from
// a memory map.
//
// Why a linear model on a device: scoring an utterance is adding up a few rows of an int8 table, which takes
// microseconds, needs no floating-point hardware beyond a final softmax, and has a size and latency you can
// state in advance. docs/DESIGN.md has the file format and what was measured.
//
// The file format (little-endian, version 1). Offsets are from the start of the file.
//
//    0   char     magic[4]        "PMDL"
//    4   u32      version         1
//    8   u32      n_classes       1 to 4096
//   12   u32      n_features      a power of two, 64 to 2^24 (feature hash & (n_features - 1) picks the row)
//   16   u32      flags           bit 0: weights are int8 with a per-class scale. Otherwise float32.
//   20   u32      names_offset    n_classes entries of { u16 length, bytes }
//   24   u32      names_bytes
//   28   u32      scales_offset   float32[n_classes], only when int8
//   32   u32      bias_offset     float32[n_classes]
//   36   u32      weights_offset  64-byte aligned for int8, 4-byte aligned for float32
//   40   u64      weights_bytes   n_features * n_classes * element size
//   48   u8[16]   reserved        zero
//   ...  the sections
//   end  u32      crc32 of every byte before it
//
// Weights are stored feature-major: row f holds the n_classes weights for feature f. A sparse utterance touches a
// handful of rows, and each row is one contiguous run of bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace assist {

inline constexpr std::uint32_t kMaxClasses = 4096;
inline constexpr std::uint32_t kMinFeatures = 64;
inline constexpr std::uint32_t kMaxFeatureCount = 1U << 24;

enum class LoadStatus {
    Ok,
    IoError,
    TooSmall,
    BadMagic,
    BadVersion,
    BadHeader,    // a count, a flag or a number outside what the format allows
    BadLayout,    // a section that does not fit in the file or is not aligned
    BadChecksum,
};

const char* to_string(LoadStatus status) noexcept;

struct Prediction {
    std::uint32_t intent = 0;
    float confidence = 0.0F;  // probability of the best class
    float margin = 0.0F;      // best probability minus second best
};

class Model {
public:
    // Memory-maps the file and uses it in place. `verify_checksum` reads every byte once, which touches every page;
    // leave it off when start-up time matters more than catching a corrupted file.
    static LoadStatus load(const std::filesystem::path& path, Model& out, bool verify_checksum = true);
    // Copies the bytes first, so the span may be unaligned or short-lived. Used by tests and the fuzzer.
    static LoadStatus from_bytes(std::span<const std::byte> bytes, Model& out, bool verify_checksum = true);

    std::uint32_t class_count() const noexcept { return n_classes_; }
    std::uint32_t feature_count() const noexcept { return n_features_; }
    bool quantized() const noexcept { return int8_; }
    std::size_t file_bytes() const noexcept { return file_bytes_; }
    std::string_view class_name(std::uint32_t c) const noexcept;

    // Working space for predict(). Create one per thread, once, and reuse it: predict() then allocates nothing.
    struct Scratch {
        std::vector<std::int32_t> acc_int;
        std::vector<float> acc_float;
        std::vector<float> prob;
    };
    Scratch make_scratch() const;

    // Class probabilities for the given feature hashes, in a span that points into `scratch`.
    std::span<const float> probabilities(std::span<const std::uint32_t> features, Scratch& scratch) const noexcept;
    Prediction predict(std::span<const std::uint32_t> features, Scratch& scratch) const noexcept;

private:
    struct NameRef {
        std::uint32_t offset;
        std::uint32_t length;
    };

    std::shared_ptr<const void> owner_;  // keeps the mapping or the copied bytes alive
    const std::byte* base_ = nullptr;
    std::size_t file_bytes_ = 0;
    std::uint32_t n_classes_ = 0;
    std::uint32_t n_features_ = 0;
    bool int8_ = false;
    const float* scales_ = nullptr;
    const float* bias_ = nullptr;
    const void* weights_ = nullptr;
    std::vector<NameRef> names_;

    static LoadStatus parse(const std::byte* base, std::size_t size, bool verify_checksum, Model& out);
};

}  // namespace assist
