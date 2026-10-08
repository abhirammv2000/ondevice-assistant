// Feeds arbitrary text through every parser and then through the whole engine, checking facts that must always hold.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "assist/calc.hpp"
#include "assist/engine.hpp"
#include "assist/json.hpp"
#include "assist/redact.hpp"
#include "assist/slots.hpp"
#include "assist/tokenizer.hpp"
#include "model_builder.hpp"

namespace {

[[noreturn]] void broken() { __builtin_trap(); }
void require(bool ok) {
    if (!ok) broken();
}

// Structural check of the JSON writer's output: valid UTF-8, no raw control characters, one object.
void check_json(const std::string& j) {
    require(j.size() >= 2 && j.front() == '{' && j.back() == '}');
    std::size_t i = 0;
    while (i < j.size()) {
        const auto c = static_cast<unsigned char>(j[i]);
        require(c >= 0x20);
        std::size_t extra = 0;
        if (c < 0x80) extra = 0;
        else if (c >= 0xC2 && c <= 0xDF) extra = 1;
        else if (c >= 0xE0 && c <= 0xEF) extra = 2;
        else if (c >= 0xF0 && c <= 0xF4) extra = 3;
        else broken();
        require(i + extra < j.size());
        for (std::size_t k = 1; k <= extra; ++k) require((static_cast<unsigned char>(j[i + k]) & 0xC0) == 0x80);
        i += extra + 1;
    }
}

assist::Engine& engine() {
    static assist::Engine instance = [] {
        assist::Engine e(testutil::stub_model(), std::make_shared<assist::FixedClock>(assist::LocalTime{2026, 10, 8, 9, 15, 0}), {});
        e.set_contacts({{1, "Mom"}, {2, "Dana Whitfield"}, {3, "Jose Garcia"}});
        return e;
    }();
    return instance;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace assist;
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    TokenScratch scratch;
    extract_features(text, scratch);
    require(scratch.token_count <= kMaxTokens && scratch.feature_count <= kMaxFeatures);
    for (std::uint32_t i = 1; i < scratch.feature_count; ++i) require(scratch.features[i - 1] < scratch.features[i]);

    SlotText slots;
    slots.lex(text);
    const LocalTime now{2026, 10, 8, 9, 15, 0};
    for (std::size_t from = 0; from <= slots.size(); from += 1) {
        if (const auto d = find_duration(slots.tokens(), from)) require(d->begin <= d->end && d->end <= slots.size() && d->seconds > 0 && d->seconds <= 366 * 86400);
        if (const auto t = find_time_of_day(slots.tokens(), from)) {
            require(t->begin <= t->end && t->end <= slots.size() && t->hour < 24 && t->minute < 60);
            const LocalTime next = next_occurrence(now, *t);
            require(next.month >= 1 && next.month <= 12 && next.day >= 1 && next.day <= days_in_month(next.year, next.month));
        }
        if (const auto d = find_date(slots.tokens(), now, from)) require(d->begin <= d->end && d->end <= slots.size());
        if (from < slots.size()) {
            if (const auto n = parse_number(slots.tokens(), from)) require(n->end > from && n->end <= slots.size());
        }
    }
    const CalcResult calc = evaluate_spoken(slots.tokens());
    if (calc.status == CalcStatus::Ok) {
        require(calc.begin < calc.end && calc.end <= slots.size());
        (void)format_number(calc.value);
    }

    Redactor redactor;
    redactor.add_name("Dana Whitfield");
    (void)redactor.redact(text);

    const Result result = engine().handle(text);
    require(result.confidence >= 0.0F && result.confidence <= 1.0F);
    check_json(to_json(result));
    (void)engine().classify(text);
    return 0;
}
