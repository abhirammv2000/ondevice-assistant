// Feeds arbitrary bytes to the model loader. A model file comes from outside the process (a download, an update), so
// the loader must never read outside the buffer, whatever the header says. If the bytes do parse, scoring with them
// must also stay in bounds.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "assist/model.hpp"
#include "assist/tokenizer.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace assist;
    // the checksum is skipped, because random bytes would almost never pass it and the parser would never be reached
    for (const bool verify : {false, true}) {
        Model model;
        const auto status = Model::from_bytes({reinterpret_cast<const std::byte*>(data), size}, model, verify);
        if (status != LoadStatus::Ok) continue;
        auto scratch = model.make_scratch();
        TokenScratch tokens;
        for (const char* text : {"set a timer for ten minutes", "", "call mom", "1 2 3"}) {
            extract_features(text, tokens);
            const Prediction p = model.predict(tokens.feature_span(), scratch);
            if (p.intent >= model.class_count()) __builtin_trap();
        }
        for (std::uint32_t c = 0; c < model.class_count(); ++c) (void)model.class_name(c);
    }
    return 0;
}
