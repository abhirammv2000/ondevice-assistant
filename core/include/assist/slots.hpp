// Finding the numbers, durations, times and dates in an utterance.
//
// "set a timer for an hour and a half", "wake me at quarter to seven tomorrow" and "remind me on the 3rd of May"
// all carry values the device has to act on. This is a small hand-written grammar over a token list, with no
// regular expressions, no allocation and no machine-learned part, so it behaves the same way every time and can
// be tested exhaustively. The model decides what the user wants. These functions find the values.
//
// Conventions that are a matter of taste are listed in docs/DESIGN.md, for example that "friday" means the next
// Friday after today and "next friday" means the one after that.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "assist/calendar.hpp"

namespace assist {

enum class TokKind : std::uint8_t {
    Word,     // letters and apostrophes, lower-cased
    Number,   // 12, 7.5
    Clock,    // 7:30, with the parts in hour and minute
    Ordinal,  // 3rd, 21st
};

struct SlotToken {
    TokKind kind = TokKind::Word;
    std::string_view text;  // points into the SlotText that holds it
    double value = 0;       // Number and Ordinal
    unsigned hour = 0;      // Clock
    unsigned minute = 0;    // Clock
};

// The tokens of one utterance, in a fixed-size buffer. It cannot be copied because the tokens point into it.
class SlotText {
public:
    static constexpr std::size_t kMaxChars = 512;
    static constexpr std::size_t kMaxTokens = 64;

    SlotText() = default;
    SlotText(const SlotText&) = delete;
    SlotText& operator=(const SlotText&) = delete;

    // Lex the utterance. Input past the limits is ignored. "5pm" is the two tokens 5 and pm, "3rd" is one ordinal,
    // "7:30" is one clock token and "a.m." is the word am.
    void lex(std::string_view utterance) noexcept;

    std::span<const SlotToken> tokens() const noexcept { return {tok_, n_}; }
    std::size_t size() const noexcept { return n_; }
    const SlotToken& operator[](std::size_t i) const noexcept { return tok_[i]; }

private:
    char buf_[kMaxChars];
    SlotToken tok_[kMaxTokens];
    std::size_t n_ = 0;
};

using Tokens = std::span<const SlotToken>;

struct NumberMatch {
    double value;
    std::size_t end;  // one past the last token used
};

// A number written as digits ("12", "7.5") or in words ("twenty five", "one hundred and five", "two thousand").
// Words give whole numbers below a million. A teen or a tens word does not combine with a unit before it, so
// "five thirty" is the number 5 and 30 is a separate number, which is what "at five thirty" needs.
std::optional<NumberMatch> parse_number(Tokens t, std::size_t pos) noexcept;

struct DurationMatch {
    std::int64_t seconds;
    std::size_t begin;
    std::size_t end;
};

// The first duration in the tokens: "10 minutes", "an hour and a half", "half an hour", "quarter of an hour",
// "two and a half hours", "1 hour 30 minutes", "90 seconds". Units are seconds, minutes, hours, days and weeks.
// Each unit may appear once, and the total must be positive and at most 366 days.
std::optional<DurationMatch> find_duration(Tokens t, std::size_t from = 0) noexcept;

struct TimeOfDayMatch {
    unsigned hour;     // 0 to 23, after am and pm have been applied
    unsigned minute;   // 0 to 59
    bool ambiguous;    // no am, pm or part of the day was given for an hour that could be either, as in "at 7"
    std::size_t begin;
    std::size_t end;
};

// The first time of day: "7:30", "5 pm", "at 7", "noon", "midnight", "half past six", "quarter to nine",
// "ten to three", "at five thirty", "7 o'clock", with an optional "am", "pm", "in the morning", "in the evening"
// or "tonight" after it.
std::optional<TimeOfDayMatch> find_time_of_day(Tokens t, std::size_t from = 0) noexcept;

struct DateMatch {
    std::int64_t day;  // days since 1970-01-01
    std::size_t begin;
    std::size_t end;
};

// The first date, resolved against `now`: "today", "tonight", "tomorrow", "the day after tomorrow", "yesterday",
// "friday", "next friday", "in three days", "in two weeks", "may 3rd", "3 may", "the 3rd of may 2027". A month and day
// without a year is the next one on or after today. A date that does not exist, like 30 February, is not a match.
std::optional<DateMatch> find_date(Tokens t, const LocalTime& now, std::size_t from = 0) noexcept;

// The first moment after `now` at which the wall clock reads the given time. An ambiguous hour can be morning or
// evening, so the earlier of the two is chosen. A time equal to `now` counts as passed and gives tomorrow.
LocalTime next_occurrence(const LocalTime& now, const TimeOfDayMatch& when) noexcept;

}  // namespace assist
