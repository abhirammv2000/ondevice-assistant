#include <string>
#include <vector>

#include "assist/slots.hpp"
#include "doctest.h"

using namespace assist;

namespace {

struct Lexed {
    SlotText text;
    explicit Lexed(std::string_view s) { text.lex(s); }
    Tokens tokens() const { return text.tokens(); }
};

// 2026-10-08 is a Thursday
const LocalTime kNow{2026, 10, 8, 9, 15, 0};

std::int64_t day_of(std::int64_t y, unsigned m, unsigned d) { return days_from_civil(y, m, d); }

}  // namespace

// lexer

TEST_CASE("lexer: words, numbers, clock times, ordinals and am/pm") {
    Lexed l("Set a timer for 10 minutes at 7:30PM on May 3rd, 2027");
    const auto t = l.tokens();
    REQUIRE(t.size() == 13);
    CHECK(t[0].text == "set");
    CHECK(t[4].kind == TokKind::Number);
    CHECK(t[4].value == 10);
    CHECK(t[7].kind == TokKind::Clock);
    CHECK(t[7].hour == 7);
    CHECK(t[7].minute == 30);
    CHECK(t[8].text == "pm");
    CHECK(t[10].text == "may");
    CHECK(t[11].kind == TokKind::Ordinal);
    CHECK(t[11].value == 3);
    CHECK(t[12].value == 2027);
}

TEST_CASE("lexer: attached suffixes split, decimals and a.m. work") {
    {
        Lexed l("5pm and 10min");
        const auto t = l.tokens();
        REQUIRE(t.size() == 5);
        CHECK(t[0].value == 5);
        CHECK(t[1].text == "pm");
        CHECK(t[3].value == 10);
        CHECK(t[4].text == "min");
    }
    {
        Lexed l("1.5 hours at 9 a.m. or 10 P.M.");
        const auto t = l.tokens();
        CHECK(t[0].value == 1.5);
        bool am = false, pm = false;
        for (const auto& k : t) {
            am = am || k.text == "am";
            pm = pm || k.text == "pm";
        }
        CHECK(am);
        CHECK(pm);
    }
    {
        Lexed l("the 21st, 22nd, 23rd and 24th");
        int ordinals = 0;
        for (const auto& k : l.tokens()) ordinals += k.kind == TokKind::Ordinal;
        CHECK(ordinals == 4);
    }
}

TEST_CASE("lexer: hyphens split words, apostrophes stay, a stray colon is not a clock") {
    Lexed l("twenty-five o'clock 7:5 12:345");
    const auto t = l.tokens();
    CHECK(t[0].text == "twenty");
    CHECK(t[1].text == "five");
    CHECK(t[2].text == "o'clock");
    CHECK(t[3].kind == TokKind::Number);  // 7
    CHECK(t[4].kind == TokKind::Number);  // 5
    for (const auto& k : t) CHECK(k.kind != TokKind::Clock);
}

TEST_CASE("lexer: limits and hostile input do not overflow") {
    std::string many;
    for (int i = 0; i < 200; ++i) many += "a ";
    Lexed l(many);
    CHECK(l.text.size() == SlotText::kMaxTokens);

    Lexed big(std::string(5000, '9'));
    CHECK(big.text.size() <= 1);

    Lexed odd("\xff\xfe 99999999999999999999999 .... ::: ''' ...");
    CHECK(odd.text.size() <= SlotText::kMaxTokens);

    Lexed empty("");
    CHECK(empty.text.size() == 0);
}

// numbers

TEST_CASE("numbers: digits and words") {
    struct Case {
        const char* text;
        double value;
        std::size_t end;
    };
    const Case cases[] = {
        {"12", 12, 1},          {"7.5", 7.5, 1},         {"zero", 0, 1},          {"five", 5, 1},
        {"fifteen", 15, 1},     {"twenty five", 25, 2},  {"ninety nine", 99, 2},  {"one hundred", 100, 2},
        {"hundred", 100, 1},    {"one hundred and five", 105, 4}, {"two hundred fifty", 250, 3},
        {"one thousand", 1000, 2}, {"two thousand twenty six", 2026, 4}, {"five thirty", 5, 1},
        {"one two", 1, 1},      {"twenty fifteen", 20, 1}, {"forty two apples", 42, 2},
    };
    for (const auto& c : cases) {
        Lexed l(c.text);
        const auto n = parse_number(l.tokens(), 0);
        REQUIRE_MESSAGE(n.has_value(), c.text);
        CHECK_MESSAGE(n->value == c.value, c.text);
        CHECK_MESSAGE(n->end == c.end, c.text);
    }
}

TEST_CASE("numbers: things that are not numbers") {
    for (const char* text : {"", "hello", "and five", "and", "a"}) {
        Lexed l(text);
        CHECK_MESSAGE(!parse_number(l.tokens(), 0).has_value(), text);
    }
    Lexed l("five");
    CHECK_FALSE(parse_number(l.tokens(), 1).has_value());  // past the end
}

// durations

TEST_CASE("durations: the ways people say them") {
    struct Case {
        const char* text;
        std::int64_t seconds;
    };
    const Case cases[] = {
        {"10 minutes", 600},
        {"ten minutes", 600},
        {"a minute", 60},
        {"an hour", 3600},
        {"half an hour", 1800},
        {"half a minute", 30},
        {"half hour", 1800},
        {"an hour and a half", 5400},
        {"two and a half hours", 9000},
        {"1 hour 30 minutes", 5400},
        {"1 hour and 30 minutes", 5400},
        {"an hour and 20 minutes", 4800},
        {"90 seconds", 90},
        {"a quarter of an hour", 900},
        {"quarter of an hour", 900},
        {"1.5 hours", 5400},
        {"45 mins", 2700},
        {"2 hrs", 7200},
        {"three days", 259200},
        {"a week", 604800},
        {"two weeks", 1209600},
        {"one hundred and twenty seconds", 120},
        {"2 hours 15 minutes 30 seconds", 8130},
    };
    for (const auto& c : cases) {
        Lexed l(c.text);
        const auto d = find_duration(l.tokens());
        REQUIRE_MESSAGE(d.has_value(), c.text);
        CHECK_MESSAGE(d->seconds == c.seconds, c.text);
    }
}

TEST_CASE("durations: found inside a sentence, with the position of the phrase") {
    Lexed l("set a timer for an hour and a half please");
    const auto d = find_duration(l.tokens());
    REQUIRE(d.has_value());
    CHECK(d->seconds == 5400);
    CHECK(d->begin == 4);
    CHECK(d->end == 9);
}

TEST_CASE("durations: things that are not durations") {
    for (const char* text : {"", "ten", "minutes", "0 minutes", "a timer", "400 days", "hello world", "half"}) {
        Lexed l(text);
        CHECK_MESSAGE(!find_duration(l.tokens()).has_value(), text);
    }
}

TEST_CASE("durations: a unit used twice stops at the second use") {
    Lexed l("two hours two hours");
    const auto d = find_duration(l.tokens());
    REQUIRE(d.has_value());
    CHECK(d->seconds == 7200);
    CHECK(d->end == 2);
}

TEST_CASE("durations: whatever the hours, minutes and seconds, spelled in digits, they add up") {
    std::uint32_t state = 12345;
    auto next = [&] {
        state = state * 1664525U + 1013904223U;
        return state >> 8;
    };
    for (int i = 0; i < 2000; ++i) {
        const unsigned h = next() % 24, m = next() % 60, s = next() % 60;
        std::string text;
        if (h) text += std::to_string(h) + (h == 1 ? " hour " : " hours ");
        if (m) text += std::to_string(m) + (m == 1 ? " minute " : " minutes ");
        if (s) text += std::to_string(s) + (s == 1 ? " second" : " seconds");
        if (text.empty()) continue;
        Lexed l(text);
        const auto d = find_duration(l.tokens());
        REQUIRE_MESSAGE(d.has_value(), text);
        CHECK_MESSAGE(d->seconds == h * 3600 + m * 60 + s, text);
    }
}

// times of day

TEST_CASE("times: clock readings and what is left ambiguous") {
    struct Case {
        const char* text;
        unsigned hour, minute;
        bool ambiguous;
    };
    const Case cases[] = {
        {"7:30 pm", 19, 30, false},     {"7:30", 7, 30, true},         {"13:45", 13, 45, false},
        {"0:15", 0, 15, false},         {"5 pm", 17, 0, false},        {"5pm", 17, 0, false},
        {"12 am", 0, 0, false},         {"12 pm", 12, 0, false},       {"11 pm", 23, 0, false},
        {"at 7", 7, 0, true},           {"at 17", 17, 0, false},       {"at 12", 12, 0, true},
        {"noon", 12, 0, false},         {"at noon", 12, 0, false},     {"midnight", 0, 0, false},
        {"half past six", 6, 30, true}, {"quarter past six", 6, 15, true}, {"quarter to seven", 6, 45, true},
        {"a quarter to seven", 6, 45, true}, {"ten to three", 2, 50, true}, {"twenty past five", 5, 20, true},
        {"quarter to one", 12, 45, true}, {"five to twelve", 11, 55, true},
        {"at five thirty", 5, 30, true}, {"at six oh five", 6, 5, true}, {"at seven fifteen", 7, 15, true},
        {"7 o'clock", 7, 0, true},      {"at 9 in the morning", 9, 0, false}, {"at 9 in the evening", 21, 0, false},
        {"7 in the morning", 7, 0, false}, {"at 8 tonight", 20, 0, false}, {"at 12 at night", 12, 0, false},
        {"6:45 in the afternoon", 18, 45, false}, {"twenty past five in the evening", 17, 20, false},
    };
    for (const auto& c : cases) {
        Lexed l(c.text);
        const auto t = find_time_of_day(l.tokens());
        REQUIRE_MESSAGE(t.has_value(), c.text);
        CHECK_MESSAGE(t->hour == c.hour, c.text);
        CHECK_MESSAGE(t->minute == c.minute, c.text);
        CHECK_MESSAGE(t->ambiguous == c.ambiguous, c.text);
    }
}

TEST_CASE("times: the matched range includes 'at' and the part of the day") {
    Lexed l("wake me at 7:30 pm tomorrow");
    const auto t = find_time_of_day(l.tokens());
    REQUIRE(t.has_value());
    CHECK(t->begin == 2);
    CHECK(t->end == 5);

    Lexed m("see you at nine in the evening ok");
    const auto u = find_time_of_day(m.tokens());
    REQUIRE(u.has_value());
    CHECK(u->begin == 2);
    CHECK(u->end == 7);
}

TEST_CASE("times: things that are not times of day") {
    for (const char* text : {"", "ten minutes", "at 5 minutes", "at twenty five", "13 pm", "25:00", "7:75", "hello", "at the park", "set a timer for 10 minutes"}) {
        Lexed l(text);
        CHECK_MESSAGE(!find_time_of_day(l.tokens()).has_value(), text);
    }
}

TEST_CASE("times: a range like '1 to 3 pm' finds the 3 pm and not a made-up 1:59") {
    Lexed l("from 1 to 3 pm");
    const auto t = find_time_of_day(l.tokens());
    REQUIRE(t.has_value());
    CHECK(t->hour == 15);
    CHECK(t->minute == 0);
}

// next occurrence

TEST_CASE("next occurrence: the first matching moment strictly after now") {
    auto at = [&](unsigned h, unsigned m, bool amb) { return next_occurrence(kNow, TimeOfDayMatch{h, m, amb, 0, 0}); };
    CHECK(at(7, 30, false) == LocalTime{2026, 10, 9, 7, 30, 0});   // already passed today, so tomorrow
    CHECK(at(10, 0, false) == LocalTime{2026, 10, 8, 10, 0, 0});
    CHECK(at(9, 15, false) == LocalTime{2026, 10, 9, 9, 15, 0});   // equal to now counts as passed
    CHECK(at(9, 16, false) == LocalTime{2026, 10, 8, 9, 16, 0});
    CHECK(at(7, 30, true) == LocalTime{2026, 10, 8, 19, 30, 0});   // 7:30 am has passed, so 7:30 pm
    CHECK(at(9, 30, true) == LocalTime{2026, 10, 8, 9, 30, 0});    // the morning one is still ahead
    CHECK(at(12, 0, true) == LocalTime{2026, 10, 8, 12, 0, 0});    // noon, not the midnight that has passed
}

TEST_CASE("next occurrence: across midnight, month end and year end") {
    const LocalTime late{2026, 12, 31, 23, 30, 0};
    CHECK(next_occurrence(late, TimeOfDayMatch{6, 0, true, 0, 0}) == LocalTime{2027, 1, 1, 6, 0, 0});
    CHECK(next_occurrence(late, TimeOfDayMatch{12, 0, true, 0, 0}) == LocalTime{2027, 1, 1, 0, 0, 0});
    const LocalTime leap{2028, 2, 28, 23, 59, 0};
    CHECK(next_occurrence(leap, TimeOfDayMatch{1, 0, false, 0, 0}) == LocalTime{2028, 2, 29, 1, 0, 0});
}

TEST_CASE("next occurrence: always in the future and within a day, whatever the inputs") {
    std::uint32_t state = 99;
    auto next = [&] {
        state = state * 1664525U + 1013904223U;
        return state >> 8;
    };
    for (int i = 0; i < 5000; ++i) {
        const LocalTime now = LocalTime::from_seconds(static_cast<std::int64_t>(next() % 4000000000ULL) - 1000000000LL);
        const TimeOfDayMatch when{next() % 24, next() % 60, (next() & 1) != 0, 0, 0};
        const LocalTime got = next_occurrence(now, when);
        REQUIRE(got.seconds() > now.seconds());
        REQUIRE(got.seconds() - now.seconds() <= 86400);
        REQUIRE(got.minute == when.minute);
        REQUIRE(got.second == 0);
        if (!when.ambiguous) {
            REQUIRE(got.hour == when.hour);
        } else {
            REQUIRE((got.hour == when.hour % 12 || got.hour == when.hour % 12 + 12));
        }
    }
}

// dates

TEST_CASE("dates: relative words and weekdays, from a Thursday") {
    struct Case {
        const char* text;
        std::int64_t day;
    };
    const std::int64_t today = kNow.days();
    const Case cases[] = {
        {"today", today},           {"tonight", today},        {"tomorrow", today + 1},
        {"the day after tomorrow", today + 2}, {"yesterday", today - 1},
        {"friday", today + 1},      {"on friday", today + 1},  {"saturday", today + 2},
        {"monday", today + 4},      {"thursday", today + 7},   {"this saturday", today + 2},
        {"next friday", today + 8}, {"next thursday", today + 14},
        {"in three days", today + 3}, {"in a week", today + 7}, {"in two weeks", today + 14},
        {"in 10 days", today + 10},
    };
    for (const auto& c : cases) {
        Lexed l(c.text);
        const auto d = find_date(l.tokens(), kNow);
        REQUIRE_MESSAGE(d.has_value(), c.text);
        CHECK_MESSAGE(d->day == c.day, c.text);
    }
}

TEST_CASE("dates: a month and day, in either order, with or without a year") {
    struct Case {
        const char* text;
        std::int64_t day;
    };
    const Case cases[] = {
        {"may 3rd", day_of(2027, 5, 3)},        {"may 3", day_of(2027, 5, 3)},
        {"3 may", day_of(2027, 5, 3)},          {"3rd of may", day_of(2027, 5, 3)},
        {"the third of may", day_of(2027, 5, 3)}, {"the 3rd of may 2028", day_of(2028, 5, 3)},
        {"may 3 2028", day_of(2028, 5, 3)},     {"october 8", day_of(2026, 10, 8)},     // today counts
        {"october 7", day_of(2027, 10, 7)},     {"december thirty first", day_of(2026, 12, 31)},
        {"on the twenty first of december", day_of(2026, 12, 21)}, {"february 29", day_of(2028, 2, 29)},
        {"sept 15", day_of(2027, 9, 15)},       {"jan 1st", day_of(2027, 1, 1)},
    };
    for (const auto& c : cases) {
        Lexed l(c.text);
        const auto d = find_date(l.tokens(), kNow);
        REQUIRE_MESSAGE(d.has_value(), c.text);
        CHECK_MESSAGE(d->day == c.day, c.text);
    }
}

TEST_CASE("dates: impossible dates and non-dates are not matches") {
    for (const char* text : {"", "february 30", "april 31", "february 29 2027", "may i", "may", "march", "the weather", "this is", "in the park", "next"}) {
        Lexed l(text);
        CHECK_MESSAGE(!find_date(l.tokens(), kNow).has_value(), text);
    }
}

TEST_CASE("dates: the range includes 'on'") {
    Lexed l("remind me on friday to call mom");
    const auto d = find_date(l.tokens(), kNow);
    REQUIRE(d.has_value());
    CHECK(d->begin == 2);
    CHECK(d->end == 4);
}

TEST_CASE("dates: a leap day is found even when it is years away") {
    const LocalTime now{2029, 3, 1, 0, 0, 0};
    Lexed l("february 29");
    const auto d = find_date(l.tokens(), now);
    REQUIRE(d.has_value());
    CHECK(d->day == day_of(2032, 2, 29));
}

TEST_CASE("lexer: thousands separators join, other commas do not") {
    auto numbers = [](std::string_view text) {
        Lexed l(text);
        std::vector<double> out;
        for (const auto& k : l.tokens()) {
            if (k.kind == TokKind::Number) out.push_back(k.value);
        }
        return out;
    };
    CHECK(numbers("25% of $54,788") == std::vector<double>{25, 54788});
    CHECK(numbers("1,000,000 dollars") == std::vector<double>{1000000});
    CHECK(numbers("12,34") == std::vector<double>{12, 34});
    CHECK(numbers("1,2345") == std::vector<double>{1, 2345});
    CHECK(numbers("1234,567") == std::vector<double>{1234, 567});
    CHECK(numbers("5, 6, 7") == std::vector<double>{5, 6, 7});
    CHECK(numbers("3,500.25") == std::vector<double>{3500.25});
}

TEST_CASE("lexer: % + and * become the words they stand for") {
    Lexed l("15% of 68 and 5+3 and 4*2");
    std::string joined;
    for (const auto& k : l.tokens()) joined += std::string(k.text) + "|";
    CHECK(joined == "15|percent|of|68|and|5|plus|3|and|4|times|2|");
}
