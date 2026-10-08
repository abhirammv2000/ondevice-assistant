// Builds a tiny model in memory for tests, so the routing logic can be tested without depending on what a trained
// model happens to predict. Each trigger word pushes any utterance that contains it towards one class.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "assist/hash.hpp"
#include "assist/model.hpp"

namespace testutil {

inline void put(std::vector<std::byte>& b, const void* data, std::size_t n) {
    const auto* p = static_cast<const std::byte*>(data);
    b.insert(b.end(), p, p + n);
}

inline void pad(std::vector<std::byte>& b, std::size_t alignment) {
    while (b.size() % alignment != 0) b.push_back(std::byte{0});
}

// Float32 model file bytes, in the format core/include/assist/model.hpp describes.
inline std::vector<std::byte> build_trigger_model(const std::vector<std::string>& classes,
                                                  const std::vector<std::pair<std::string, std::string>>& triggers,
                                                  std::uint32_t n_features = 4096, float weight = 40.0F) {
    const std::uint32_t n_classes = static_cast<std::uint32_t>(classes.size());
    std::vector<float> weights(static_cast<std::size_t>(n_features) * n_classes, 0.0F);
    for (const auto& [word, cls] : triggers) {
        std::uint32_t c = 0;
        while (c < n_classes && classes[c] != cls) ++c;
        if (c == n_classes) continue;
        const std::uint32_t row = assist::fold32(assist::fnv1a_append(assist::fnv1a_append(assist::kFnvOffset, "u:"), word)) & (n_features - 1);
        weights[static_cast<std::size_t>(row) * n_classes + c] += weight;
    }
    const std::vector<float> bias(n_classes, 0.0F);

    std::vector<std::byte> b(64, std::byte{0});
    const std::uint32_t names_offset = static_cast<std::uint32_t>(b.size());
    for (const auto& name : classes) {
        const std::uint16_t len = static_cast<std::uint16_t>(name.size());
        put(b, &len, 2);
        put(b, name.data(), name.size());
    }
    const std::uint32_t names_bytes = static_cast<std::uint32_t>(b.size()) - names_offset;
    pad(b, 4);
    const std::uint32_t bias_offset = static_cast<std::uint32_t>(b.size());
    put(b, bias.data(), bias.size() * sizeof(float));
    pad(b, 4);
    const std::uint32_t weights_offset = static_cast<std::uint32_t>(b.size());
    put(b, weights.data(), weights.size() * sizeof(float));
    pad(b, 4);

    const std::uint32_t version = 1, flags = 0, scales_offset = 0;
    const std::uint64_t weights_bytes = static_cast<std::uint64_t>(weights.size()) * sizeof(float);
    std::memcpy(b.data() + 0, "PMDL", 4);
    std::memcpy(b.data() + 4, &version, 4);
    std::memcpy(b.data() + 8, &n_classes, 4);
    std::memcpy(b.data() + 12, &n_features, 4);
    std::memcpy(b.data() + 16, &flags, 4);
    std::memcpy(b.data() + 20, &names_offset, 4);
    std::memcpy(b.data() + 24, &names_bytes, 4);
    std::memcpy(b.data() + 28, &scales_offset, 4);
    std::memcpy(b.data() + 32, &bias_offset, 4);
    std::memcpy(b.data() + 36, &weights_offset, 4);
    std::memcpy(b.data() + 40, &weights_bytes, 8);
    const std::uint32_t crc = assist::crc32({b.data(), b.size()});
    put(b, &crc, 4);
    return b;
}

// The model the engine tests use: fifteen classes named like the real intents, each pulled by a trigger word.
inline std::vector<std::byte> stub_model_bytes() {
    const std::vector<std::string> classes{"timer", "alarm", "time", "date", "calculator", "flip_coin", "roll_dice", "make_call",
                                           "text", "reminder_update", "greeting", "goodbye", "thank_you", "weather", "other"};
    const std::vector<std::pair<std::string, std::string>> triggers{
        {"timer", "timer"}, {"wake", "alarm"}, {"alarm", "alarm"}, {"time", "time"}, {"date", "date"}, {"calculate", "calculator"},
        {"coin", "flip_coin"}, {"flip", "flip_coin"}, {"dice", "roll_dice"}, {"die", "roll_dice"}, {"roll", "roll_dice"},
        {"call", "make_call"}, {"text", "text"}, {"remind", "reminder_update"}, {"hello", "greeting"}, {"goodbye", "goodbye"},
        {"thanks", "thank_you"}, {"weather", "weather"}};
    return build_trigger_model(classes, triggers);
}

inline assist::Model stub_model() {
    const auto bytes = stub_model_bytes();
    assist::Model m;
    if (assist::Model::from_bytes(bytes, m) != assist::LoadStatus::Ok) throw std::runtime_error("the stub model did not load");
    return m;
}

}  // namespace testutil
