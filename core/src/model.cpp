#include "assist/model.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>

#include "assist/hash.hpp"
#include "assist/mapped_file.hpp"

static_assert(std::endian::native == std::endian::little, "the model file is little-endian and is read in place");

namespace assist {
namespace {

constexpr std::size_t kHeaderBytes = 64;
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kFlagInt8 = 1U;
constexpr char kMagic[4] = {'P', 'M', 'D', 'L'};

template <class T>
T read_at(const std::byte* base, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

// offset + length <= limit, without overflowing
constexpr bool fits(std::uint64_t offset, std::uint64_t length, std::uint64_t limit) noexcept {
    return offset <= limit && length <= limit - offset;
}

}  // namespace

const char* to_string(LoadStatus status) noexcept {
    switch (status) {
        case LoadStatus::Ok: return "ok";
        case LoadStatus::IoError: return "io error";
        case LoadStatus::TooSmall: return "file too small";
        case LoadStatus::BadMagic: return "bad magic";
        case LoadStatus::BadVersion: return "unsupported version";
        case LoadStatus::BadHeader: return "invalid header";
        case LoadStatus::BadLayout: return "invalid layout";
        case LoadStatus::BadChecksum: return "checksum mismatch";
    }
    return "unknown";
}

LoadStatus Model::parse(const std::byte* base, std::size_t size, bool verify_checksum, Model& out) {
    if (size < kHeaderBytes + sizeof(std::uint32_t)) return LoadStatus::TooSmall;
    if (std::memcmp(base, kMagic, sizeof kMagic) != 0) return LoadStatus::BadMagic;
    if (read_at<std::uint32_t>(base, 4) != kVersion) return LoadStatus::BadVersion;

    const auto n_classes = read_at<std::uint32_t>(base, 8);
    const auto n_features = read_at<std::uint32_t>(base, 12);
    const auto flags = read_at<std::uint32_t>(base, 16);
    const auto names_offset = read_at<std::uint32_t>(base, 20);
    const auto names_bytes = read_at<std::uint32_t>(base, 24);
    const auto scales_offset = read_at<std::uint32_t>(base, 28);
    const auto bias_offset = read_at<std::uint32_t>(base, 32);
    const auto weights_offset = read_at<std::uint32_t>(base, 36);
    const auto weights_bytes = read_at<std::uint64_t>(base, 40);

    if (n_classes < 1 || n_classes > kMaxClasses) return LoadStatus::BadHeader;
    if (n_features < kMinFeatures || n_features > kMaxFeatureCount || !std::has_single_bit(n_features)) return LoadStatus::BadHeader;
    if ((flags & ~kFlagInt8) != 0) return LoadStatus::BadHeader;
    for (std::size_t i = 48; i < kHeaderBytes; ++i) {
        if (base[i] != std::byte{0}) return LoadStatus::BadHeader;
    }
    if (reinterpret_cast<std::uintptr_t>(base) % alignof(float) != 0) return LoadStatus::BadLayout;

    const bool int8 = (flags & kFlagInt8) != 0;
    const std::uint64_t limit = size - sizeof(std::uint32_t);  // the checksum is the last four bytes

    if (names_offset < kHeaderBytes || !fits(names_offset, names_bytes, limit)) return LoadStatus::BadLayout;
    if (bias_offset < kHeaderBytes || bias_offset % 4 != 0 || !fits(bias_offset, 4ULL * n_classes, limit)) return LoadStatus::BadLayout;
    if (int8) {
        if (scales_offset < kHeaderBytes || scales_offset % 4 != 0 || !fits(scales_offset, 4ULL * n_classes, limit)) return LoadStatus::BadLayout;
    } else if (scales_offset != 0) {
        return LoadStatus::BadHeader;
    }
    const std::uint64_t element = int8 ? 1 : 4;
    if (weights_bytes != static_cast<std::uint64_t>(n_features) * n_classes * element) return LoadStatus::BadHeader;
    if (weights_offset < kHeaderBytes || weights_offset % (int8 ? 64 : 4) != 0 || !fits(weights_offset, weights_bytes, limit)) return LoadStatus::BadLayout;

    // class names: n_classes entries of { u16 length, bytes }, filling the names section exactly
    std::vector<NameRef> names;
    names.reserve(n_classes);
    std::uint64_t cursor = names_offset;
    const std::uint64_t names_end = static_cast<std::uint64_t>(names_offset) + names_bytes;
    for (std::uint32_t c = 0; c < n_classes; ++c) {
        if (!fits(cursor, 2, names_end)) return LoadStatus::BadLayout;
        const auto length = read_at<std::uint16_t>(base, static_cast<std::size_t>(cursor));
        cursor += 2;
        if (!fits(cursor, length, names_end)) return LoadStatus::BadLayout;
        names.push_back({static_cast<std::uint32_t>(cursor), length});
        cursor += length;
    }
    if (cursor != names_end) return LoadStatus::BadLayout;

    const auto* bias = reinterpret_cast<const float*>(base + bias_offset);
    const auto* scales = int8 ? reinterpret_cast<const float*>(base + scales_offset) : nullptr;
    for (std::uint32_t c = 0; c < n_classes; ++c) {
        if (!std::isfinite(bias[c])) return LoadStatus::BadHeader;
        if (scales && !std::isfinite(scales[c])) return LoadStatus::BadHeader;
    }

    if (verify_checksum) {
        const auto stored = read_at<std::uint32_t>(base, size - sizeof(std::uint32_t));
        if (crc32({base, size - sizeof(std::uint32_t)}) != stored) return LoadStatus::BadChecksum;
        if (!int8) {  // a NaN weight would turn every probability into NaN
            const auto* w = reinterpret_cast<const float*>(base + weights_offset);
            const std::uint64_t count = weights_bytes / 4;
            for (std::uint64_t i = 0; i < count; ++i) {
                if (!std::isfinite(w[i])) return LoadStatus::BadHeader;
            }
        }
    }

    out.base_ = base;
    out.file_bytes_ = size;
    out.n_classes_ = n_classes;
    out.n_features_ = n_features;
    out.int8_ = int8;
    out.scales_ = scales;
    out.bias_ = bias;
    out.weights_ = base + weights_offset;
    out.names_ = std::move(names);
    return LoadStatus::Ok;
}

LoadStatus Model::load(const std::filesystem::path& path, Model& out, bool verify_checksum) {
    std::string error;
    auto mapped = MappedFile::open(path, &error);
    if (!mapped) return LoadStatus::IoError;
    Model model;
    const LoadStatus status = parse(mapped->data(), mapped->size(), verify_checksum, model);
    if (status != LoadStatus::Ok) return status;
    model.owner_ = std::move(mapped);
    out = std::move(model);
    return LoadStatus::Ok;
}

LoadStatus Model::from_bytes(std::span<const std::byte> bytes, Model& out, bool verify_checksum) {
    // 64-byte alignment inside a vector that is only guaranteed to be aligned for its element type
    constexpr std::size_t kAlign = 64;
    auto storage = std::make_shared<std::vector<std::byte>>(bytes.size() + kAlign);
    auto* aligned = reinterpret_cast<std::byte*>((reinterpret_cast<std::uintptr_t>(storage->data()) + kAlign - 1) & ~(static_cast<std::uintptr_t>(kAlign) - 1));
    if (!bytes.empty()) std::memcpy(aligned, bytes.data(), bytes.size());
    Model model;
    const LoadStatus status = parse(aligned, bytes.size(), verify_checksum, model);
    if (status != LoadStatus::Ok) return status;
    model.owner_ = std::move(storage);
    out = std::move(model);
    return LoadStatus::Ok;
}

std::string_view Model::class_name(std::uint32_t c) const noexcept {
    if (c >= names_.size()) return {};
    return {reinterpret_cast<const char*>(base_) + names_[c].offset, names_[c].length};
}

Model::Scratch Model::make_scratch() const {
    Scratch s;
    s.prob.assign(n_classes_, 0.0F);
    if (int8_) {
        s.acc_int.assign(n_classes_, 0);
    } else {
        s.acc_float.assign(n_classes_, 0.0F);
    }
    return s;
}

std::span<const float> Model::probabilities(std::span<const std::uint32_t> features, Scratch& s) const noexcept {
    const std::size_t classes = n_classes_;
    if (classes == 0 || s.prob.size() < classes || (int8_ ? s.acc_int.size() : s.acc_float.size()) < classes) return {};

    float* const prob = s.prob.data();
    const std::uint32_t mask = n_features_ - 1;
    // every active feature has weight 1 / sqrt(n), so the sum is taken first and scaled once at the end
    const float inv_norm = features.empty() ? 0.0F : 1.0F / std::sqrt(static_cast<float>(features.size()));

    if (int8_) {
        std::int32_t* const acc = s.acc_int.data();
        std::fill_n(acc, classes, 0);
        const auto* weights = static_cast<const std::int8_t*>(weights_);
        for (const std::uint32_t f : features) {
            const std::int8_t* row = weights + static_cast<std::size_t>(f & mask) * classes;
            for (std::size_t c = 0; c < classes; ++c) acc[c] += row[c];
        }
        for (std::size_t c = 0; c < classes; ++c) prob[c] = bias_[c] + scales_[c] * static_cast<float>(acc[c]) * inv_norm;
    } else {
        float* const acc = s.acc_float.data();
        std::fill_n(acc, classes, 0.0F);
        const auto* weights = static_cast<const float*>(weights_);
        for (const std::uint32_t f : features) {
            const float* row = weights + static_cast<std::size_t>(f & mask) * classes;
            for (std::size_t c = 0; c < classes; ++c) acc[c] += row[c];
        }
        for (std::size_t c = 0; c < classes; ++c) prob[c] = bias_[c] + acc[c] * inv_norm;
    }

    // softmax, subtracting the maximum first so exp() cannot overflow
    const float peak = *std::max_element(prob, prob + classes);
    float total = 0.0F;
    for (std::size_t c = 0; c < classes; ++c) {
        prob[c] = std::exp(prob[c] - peak);
        total += prob[c];
    }
    for (std::size_t c = 0; c < classes; ++c) prob[c] /= total;
    return {prob, classes};
}

Prediction Model::predict(std::span<const std::uint32_t> features, Scratch& scratch) const noexcept {
    const auto p = probabilities(features, scratch);
    Prediction out;
    if (p.empty()) return out;
    float best = -1.0F;
    float second = 0.0F;
    for (std::size_t c = 0; c < p.size(); ++c) {
        if (p[c] > best) {
            second = std::max(best, 0.0F);
            best = p[c];
            out.intent = static_cast<std::uint32_t>(c);
        } else if (p[c] > second) {
            second = p[c];
        }
    }
    out.confidence = best;
    out.margin = best - second;
    return out;
}

}  // namespace assist
