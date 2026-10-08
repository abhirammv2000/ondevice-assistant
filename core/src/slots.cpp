#include "assist/slots.hpp"

#include <array>
#include <cmath>
#include <limits>

namespace assist {
namespace {

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

struct WordValue {
    std::string_view word;
    int value;
};

constexpr std::array<WordValue, 10> kUnits{{{"zero", 0}, {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4},
                                            {"five", 5}, {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9}}};
constexpr std::array<WordValue, 10> kTeens{{{"ten", 10}, {"eleven", 11}, {"twelve", 12}, {"thirteen", 13}, {"fourteen", 14},
                                            {"fifteen", 15}, {"sixteen", 16}, {"seventeen", 17}, {"eighteen", 18}, {"nineteen", 19}}};
constexpr std::array<WordValue, 8> kTens{{{"twenty", 20}, {"thirty", 30}, {"forty", 40}, {"fifty", 50},
                                          {"sixty", 60}, {"seventy", 70}, {"eighty", 80}, {"ninety", 90}}};
constexpr std::array<WordValue, 21> kOrdinals{{{"first", 1}, {"second", 2}, {"third", 3}, {"fourth", 4}, {"fifth", 5},
                                               {"sixth", 6}, {"seventh", 7}, {"eighth", 8}, {"ninth", 9}, {"tenth", 10},
                                               {"eleventh", 11}, {"twelfth", 12}, {"thirteenth", 13}, {"fourteenth", 14},
                                               {"fifteenth", 15}, {"sixteenth", 16}, {"seventeenth", 17}, {"eighteenth", 18},
                                               {"nineteenth", 19}, {"twentieth", 20}, {"thirtieth", 30}}};
constexpr std::array<WordValue, 16> kMonths{{{"january", 1}, {"february", 2}, {"march", 3}, {"april", 4}, {"may", 5},
                                             {"june", 6}, {"july", 7}, {"august", 8}, {"september", 9}, {"october", 10},
                                             {"november", 11}, {"december", 12}, {"jan", 1}, {"feb", 2}, {"sept", 9}, {"sep", 9}}};
constexpr std::array<WordValue, 7> kWeekdays{{{"sunday", 0}, {"monday", 1}, {"tuesday", 2}, {"wednesday", 3},
                                              {"thursday", 4}, {"friday", 5}, {"saturday", 6}}};

template <std::size_t N>
std::optional<int> lookup(const std::array<WordValue, N>& table, std::string_view word) noexcept {
    for (const auto& e : table) {
        if (e.word == word) return e.value;
    }
    return std::nullopt;
}

bool is_word(Tokens t, std::size_t i, std::string_view w) noexcept {
    return i < t.size() && t[i].kind == TokKind::Word && t[i].text == w;
}

bool is_integer(double v) noexcept { return v >= 0 && v == std::floor(v) && v < 1e9; }

}  // namespace

// lexer

void SlotText::lex(std::string_view in) noexcept {
    n_ = 0;
    std::size_t w = 0;  // write position in buf_
    std::size_t i = 0;
    const std::size_t n = in.size();

    auto copy_lower = [&](std::size_t from, std::size_t to) -> std::string_view {
        const std::size_t start = w;
        for (std::size_t k = from; k < to && w < kMaxChars; ++k) buf_[w++] = lower(in[k]);
        return {buf_ + start, w - start};
    };
    auto word = [&](std::string_view text) -> bool {
        if (n_ >= kMaxTokens) return false;
        tok_[n_++] = SlotToken{TokKind::Word, text, 0, 0, 0};
        return true;
    };

    while (i < n && n_ < kMaxTokens) {
        const char c = in[i];
        if (is_digit(c)) {
            std::size_t j = i;
            while (j < n && is_digit(in[j])) ++j;
            std::size_t int_end = j;
            bool decimal = false;
            if (j + 1 < n && in[j] == '.' && is_digit(in[j + 1])) {
                decimal = true;
                ++j;
                while (j < n && is_digit(in[j])) ++j;
            }
            SlotToken tok;
            tok.kind = TokKind::Number;
            // at most 15 digits on each side of the point, so a long run of digits cannot overflow the double
            double value = 0;
            for (std::size_t k = i; k < int_end && k - i < 15; ++k) value = value * 10 + (in[k] - '0');
            if (decimal) {
                double place = 0.1;
                for (std::size_t k = int_end + 1; k < j && k - int_end <= 15; ++k) {
                    value += (in[k] - '0') * place;
                    place /= 10;
                }
            }
            tok.value = value;
            if (!decimal && j + 2 < n && in[j] == ':' && is_digit(in[j + 1]) && is_digit(in[j + 2]) && !(j + 3 < n && is_digit(in[j + 3]))) {
                tok.kind = TokKind::Clock;
                tok.hour = static_cast<unsigned>(value > 99 ? 99 : value);
                tok.minute = static_cast<unsigned>((in[j + 1] - '0') * 10 + (in[j + 2] - '0'));
                j += 3;
            } else if (!decimal && j + 1 < n) {
                const char a = lower(in[j]);
                const char b = lower(in[j + 1]);
                const bool suffix = (a == 's' && b == 't') || (a == 'n' && b == 'd') || (a == 'r' && b == 'd') || (a == 't' && b == 'h');
                if (suffix && !(j + 2 < n && is_alpha(in[j + 2]))) {
                    tok.kind = TokKind::Ordinal;
                    j += 2;
                }
            }
            tok.text = copy_lower(i, int_end);
            tok_[n_++] = tok;
            i = j;
        } else if (is_alpha(c)) {
            // a.m. and p.m. are one word each
            if ((lower(c) == 'a' || lower(c) == 'p') && i + 2 < n && in[i + 1] == '.' && lower(in[i + 2]) == 'm' &&
                !(i + 3 < n && is_alpha(in[i + 3]))) {
                const std::size_t start = w;
                if (w + 2 <= kMaxChars) {
                    buf_[w++] = lower(c);
                    buf_[w++] = 'm';
                }
                word({buf_ + start, w - start});
                i += 3;
                if (i < n && in[i] == '.') ++i;
                continue;
            }
            std::size_t j = i;
            while (j < n && (is_alpha(in[j]) || (in[j] == '\'' && j + 1 < n && is_alpha(in[j + 1])))) ++j;
            word(copy_lower(i, j));
            i = j;
        } else {
            ++i;
        }
    }
}

// numbers

std::optional<NumberMatch> parse_number(Tokens t, std::size_t pos) noexcept {
    if (pos >= t.size()) return std::nullopt;
    if (t[pos].kind == TokKind::Number) return NumberMatch{t[pos].value, pos + 1};

    enum class Last { None, Unit, Teen, Tens, Hundred, Thousand };
    Last last = Last::None;
    std::int64_t total = 0;
    std::int64_t current = 0;
    std::size_t p = pos;
    bool any = false;
    auto is_number_word = [&](std::size_t i) {
        if (i >= t.size() || t[i].kind != TokKind::Word) return false;
        return lookup(kUnits, t[i].text) || lookup(kTeens, t[i].text) || lookup(kTens, t[i].text);
    };
    while (p < t.size() && t[p].kind == TokKind::Word) {
        const std::string_view w = t[p].text;
        if (const auto v = lookup(kUnits, w)) {
            if (last == Last::Unit || last == Last::Teen) break;
            current += *v;
            last = Last::Unit;
        } else if (const auto v2 = lookup(kTeens, w)) {
            if (last == Last::Unit || last == Last::Teen || last == Last::Tens) break;
            current += *v2;
            last = Last::Teen;
        } else if (const auto v3 = lookup(kTens, w)) {
            if (last == Last::Unit || last == Last::Teen || last == Last::Tens) break;
            current += *v3;
            last = Last::Tens;
        } else if (w == "hundred") {
            if (last == Last::Hundred) break;
            current = (current == 0 ? 1 : current) * 100;
            last = Last::Hundred;
        } else if (w == "thousand") {
            if (last == Last::Thousand) break;
            total += (current == 0 ? 1 : current) * 1000;
            current = 0;
            last = Last::Thousand;
        } else if (w == "and") {
            if ((last == Last::Hundred || last == Last::Thousand) && is_number_word(p + 1)) {
                ++p;
                continue;
            }
            break;
        } else {
            break;
        }
        any = true;
        ++p;
    }
    if (!any) return std::nullopt;
    return NumberMatch{static_cast<double>(total + current), p};
}

// durations

namespace {

struct Unit {
    std::string_view word;
    std::int64_t seconds;
    int index;
};
constexpr std::array<Unit, 16> kDurationUnits{{{"second", 1, 0}, {"seconds", 1, 0}, {"sec", 1, 0}, {"secs", 1, 0},
                                               {"minute", 60, 1}, {"minutes", 60, 1}, {"min", 60, 1}, {"mins", 60, 1},
                                               {"hour", 3600, 2}, {"hours", 3600, 2}, {"hr", 3600, 2}, {"hrs", 3600, 2},
                                               {"day", 86400, 3}, {"days", 86400, 3}, {"week", 604800, 4}, {"weeks", 604800, 4}}};

const Unit* find_unit(Tokens t, std::size_t i) noexcept {
    if (i >= t.size() || t[i].kind != TokKind::Word) return nullptr;
    for (const auto& u : kDurationUnits) {
        if (u.word == t[i].text) return &u;
    }
    return nullptr;
}

struct Segment {
    double seconds;
    std::size_t end;
    int unit;
};

bool and_a_half(Tokens t, std::size_t i) noexcept {
    return is_word(t, i, "and") && is_word(t, i + 1, "a") && is_word(t, i + 2, "half");
}

std::optional<Segment> parse_segment(Tokens t, std::size_t p) noexcept {
    if (p >= t.size()) return std::nullopt;
    // "half an hour", "half a minute", "half hour"
    if (is_word(t, p, "half")) {
        std::size_t q = p + 1;
        if (is_word(t, q, "an") || is_word(t, q, "a")) ++q;
        if (const Unit* u = find_unit(t, q)) return Segment{0.5 * static_cast<double>(u->seconds), q + 1, u->index};
        return std::nullopt;
    }
    // "quarter of an hour", "a quarter of an hour"
    {
        std::size_t q = p;
        if (is_word(t, q, "a") && is_word(t, q + 1, "quarter")) ++q;
        if (is_word(t, q, "quarter") && is_word(t, q + 1, "of")) {
            std::size_t r = q + 2;
            if (is_word(t, r, "an") || is_word(t, r, "a")) ++r;
            if (const Unit* u = find_unit(t, r)) return Segment{0.25 * static_cast<double>(u->seconds), r + 1, u->index};
        }
    }
    // "an hour", "a minute", with an optional "and a half" after the unit
    if (is_word(t, p, "a") || is_word(t, p, "an")) {
        if (const Unit* u = find_unit(t, p + 1)) {
            double amount = 1;
            std::size_t end = p + 2;
            if (and_a_half(t, end)) {
                amount = 1.5;
                end += 3;
            }
            return Segment{amount * static_cast<double>(u->seconds), end, u->index};
        }
        return std::nullopt;
    }
    // "10 minutes", "two and a half hours"
    if (const auto num = parse_number(t, p)) {
        std::size_t q = num->end;
        double amount = num->value;
        if (and_a_half(t, q)) {
            amount += 0.5;
            q += 3;
        }
        if (const Unit* u = find_unit(t, q)) return Segment{amount * static_cast<double>(u->seconds), q + 1, u->index};
    }
    return std::nullopt;
}

constexpr std::int64_t kMaxDurationSeconds = 366LL * 86400;

}  // namespace

std::optional<DurationMatch> find_duration(Tokens t, std::size_t from) noexcept {
    for (std::size_t start = from; start < t.size(); ++start) {
        double total = 0;
        std::size_t p = start;
        bool used[5] = {false, false, false, false, false};
        bool any = false;
        while (true) {
            auto seg = parse_segment(t, p);
            if (!seg && any && is_word(t, p, "and")) seg = parse_segment(t, p + 1);  // "an hour and 20 minutes"
            if (!seg || used[seg->unit]) break;
            used[seg->unit] = true;
            total += seg->seconds;
            p = seg->end;
            any = true;
        }
        if (!any) continue;
        const double rounded = std::round(total);
        if (rounded <= 0 || rounded > static_cast<double>(kMaxDurationSeconds)) continue;
        return DurationMatch{static_cast<std::int64_t>(rounded), start, p};
    }
    return std::nullopt;
}

// times of day

namespace {

struct Period {
    int half = -1;          // 0 am, 1 pm, -1 none
    std::size_t length = 0; // tokens used
};

Period read_period(Tokens t, std::size_t i) noexcept {
    if (is_word(t, i, "am")) return {0, 1};
    if (is_word(t, i, "pm")) return {1, 1};
    if (is_word(t, i, "tonight")) return {1, 1};
    if (is_word(t, i, "in") && is_word(t, i + 1, "the")) {
        if (is_word(t, i + 2, "morning")) return {0, 3};
        if (is_word(t, i + 2, "afternoon") || is_word(t, i + 2, "evening")) return {1, 3};
    }
    if (is_word(t, i, "at") && is_word(t, i + 1, "night")) return {1, 2};
    return {};
}

// An hour 0 to 23 followed by an optional minute, and the part of the day that follows.
std::optional<TimeOfDayMatch> finish(Tokens t, unsigned hour, unsigned minute, bool twelve_hour_reading, std::size_t begin, std::size_t end) noexcept {
    if (hour > 23 || minute > 59) return std::nullopt;
    const Period period = read_period(t, end);
    bool ambiguous = false;
    if (period.half >= 0) {
        if (hour >= 1 && hour <= 12) {
            if (period.half == 1 && hour < 12) hour += 12;
            if (period.half == 0 && hour == 12) hour = 0;
        }
        end += period.length;
    } else {
        ambiguous = twelve_hour_reading && hour >= 1 && hour <= 12;
    }
    return TimeOfDayMatch{hour, minute, ambiguous, begin, end};
}

// A whole-number hour from a Number token or a number word.
std::optional<NumberMatch> parse_hour(Tokens t, std::size_t p, unsigned max_hour) noexcept {
    const auto n = parse_number(t, p);
    if (!n || !is_integer(n->value) || n->value > max_hour) return std::nullopt;
    return n;
}

std::optional<TimeOfDayMatch> parse_time_at(Tokens t, std::size_t p, bool after_at) noexcept {
    if (p >= t.size()) return std::nullopt;
    const SlotToken& k = t[p];

    if (k.kind == TokKind::Word) {
        if (k.text == "noon" || k.text == "midday") return TimeOfDayMatch{12, 0, false, p, p + 1};
        if (k.text == "midnight") return TimeOfDayMatch{0, 0, false, p, p + 1};
    }
    if (k.kind == TokKind::Clock) return finish(t, k.hour, k.minute, true, p, p + 1);

    // "half past six", "quarter past six", "quarter to nine", "a quarter to nine"
    std::size_t q = p;
    if (is_word(t, q, "a") && is_word(t, q + 1, "quarter")) ++q;
    if (is_word(t, q, "half") && is_word(t, q + 1, "past")) {
        if (const auto h = parse_hour(t, q + 2, 12)) return finish(t, static_cast<unsigned>(h->value), 30, true, p, h->end);
        return std::nullopt;
    }
    if (is_word(t, q, "quarter") && (is_word(t, q + 1, "past") || is_word(t, q + 1, "to"))) {
        const bool to = is_word(t, q + 1, "to");
        if (const auto h = parse_hour(t, q + 2, 12)) {
            unsigned hour = static_cast<unsigned>(h->value);
            if (to) hour = (hour == 0 || hour == 1) ? 12 : hour - 1;
            return finish(t, hour, to ? 45 : 15, true, p, h->end);
        }
        return std::nullopt;
    }
    // "ten past three", "twenty to five"
    // (spelled-out numbers only, so "from 1 to 3 pm" is not read as 59 minutes past two)
    if (const auto m = parse_number(t, p); m && k.kind == TokKind::Word && is_integer(m->value) && m->value >= 1 && m->value <= 59 &&
                                            (is_word(t, m->end, "past") || is_word(t, m->end, "to"))) {
        const bool to = is_word(t, m->end, "to");
        if (const auto h = parse_hour(t, m->end + 1, 12)) {
            unsigned hour = static_cast<unsigned>(h->value);
            unsigned minute = static_cast<unsigned>(m->value);
            if (to) {
                hour = (hour == 0 || hour == 1) ? 12 : hour - 1;
                minute = 60 - minute;
            }
            return finish(t, hour, minute, true, p, h->end);
        }
        return std::nullopt;
    }
    // "5 pm", "7 o'clock", and after "at": "at 7", "at 17", "at five thirty", "at six oh five"
    if (const auto h = parse_hour(t, p, 23)) {
        const unsigned hour = static_cast<unsigned>(h->value);
        const std::size_t after = h->end;
        if (is_word(t, after, "am") || is_word(t, after, "pm")) {
            if (hour < 1 || hour > 12) return std::nullopt;
            return finish(t, hour, 0, true, p, after);
        }
        if (is_word(t, after, "o'clock")) return finish(t, hour, 0, true, p, after + 1);
        // "7 in the morning", "8 tonight"
        if (hour >= 1 && hour <= 12 && read_period(t, after).half >= 0) return finish(t, hour, 0, true, p, after);
        if (after_at && !find_unit(t, after)) {  // "at 5 minutes" is not a time
            unsigned minute = 0;
            std::size_t end = after;
            if (is_word(t, after, "oh")) {
                if (const auto m = parse_number(t, after + 1); m && is_integer(m->value) && m->value >= 1 && m->value <= 9) {
                    minute = static_cast<unsigned>(m->value);
                    end = m->end;
                }
            } else if (const auto m2 = parse_number(t, after);
                       m2 && is_integer(m2->value) && m2->value >= 10 && m2->value <= 59 && t[after].kind == TokKind::Word) {
                minute = static_cast<unsigned>(m2->value);
                end = m2->end;
            }
            return finish(t, hour, minute, true, p, end);
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<TimeOfDayMatch> find_time_of_day(Tokens t, std::size_t from) noexcept {
    for (std::size_t p = from; p < t.size(); ++p) {
        if (is_word(t, p, "at")) {
            if (auto m = parse_time_at(t, p + 1, true)) {
                m->begin = p;
                return m;
            }
            continue;
        }
        if (auto m = parse_time_at(t, p, false)) return m;
    }
    return std::nullopt;
}

LocalTime next_occurrence(const LocalTime& now, const TimeOfDayMatch& when) noexcept {
    const std::int64_t now_s = now.seconds();
    const std::int64_t today = now.days() * 86400;
    unsigned hours[2];
    int count = 0;
    if (when.ambiguous) {
        hours[count++] = when.hour % 12;
        hours[count++] = when.hour % 12 + 12;
    } else {
        hours[count++] = when.hour;
    }
    std::int64_t best = std::numeric_limits<std::int64_t>::max();
    for (int offset = 0; offset <= 1; ++offset) {
        for (int i = 0; i < count; ++i) {
            const std::int64_t candidate = today + offset * 86400 + hours[i] * 3600 + when.minute * 60;
            if (candidate > now_s && candidate < best) best = candidate;
        }
    }
    return LocalTime::from_seconds(best);
}

// dates

namespace {

struct DayMatch {
    unsigned day;
    std::size_t end;
};

// A day of the month: 3rd, 3, third, twenty first
std::optional<DayMatch> parse_day(Tokens t, std::size_t p) noexcept {
    if (p >= t.size()) return std::nullopt;
    const SlotToken& k = t[p];
    if (k.kind == TokKind::Ordinal || k.kind == TokKind::Number) {
        if (!is_integer(k.value) || k.value < 1 || k.value > 31) return std::nullopt;
        return DayMatch{static_cast<unsigned>(k.value), p + 1};
    }
    if (k.kind != TokKind::Word) return std::nullopt;
    if (const auto v = lookup(kOrdinals, k.text)) return DayMatch{static_cast<unsigned>(*v), p + 1};
    if (k.text == "twenty" || k.text == "thirty") {
        if (p + 1 < t.size() && t[p + 1].kind == TokKind::Word) {
            if (const auto u = lookup(kOrdinals, t[p + 1].text); u && *u <= 9) {
                const unsigned day = (k.text == "twenty" ? 20U : 30U) + static_cast<unsigned>(*u);
                if (day <= 31) return DayMatch{day, p + 2};
            }
        }
    }
    return std::nullopt;
}

std::optional<unsigned> month_at(Tokens t, std::size_t p) noexcept {
    if (p >= t.size() || t[p].kind != TokKind::Word) return std::nullopt;
    if (const auto m = lookup(kMonths, t[p].text)) return static_cast<unsigned>(*m);
    return std::nullopt;
}

bool is_year(const SlotToken& k) noexcept { return k.kind == TokKind::Number && is_integer(k.value) && k.value >= 1900 && k.value <= 2200; }

// The day number for a month and day, in `year` if one was given and otherwise the next time it comes round.
std::optional<std::int64_t> resolve_month_day(const LocalTime& now, unsigned month, unsigned day, std::optional<std::int64_t> year) noexcept {
    if (year) {
        if (day > days_in_month(*year, month)) return std::nullopt;
        return days_from_civil(*year, month, day);
    }
    const std::int64_t today = now.days();
    for (std::int64_t y = now.year; y <= now.year + 8; ++y) {
        if (day > days_in_month(y, month)) continue;
        const std::int64_t d = days_from_civil(y, month, day);
        if (d >= today) return d;
    }
    return std::nullopt;
}

std::optional<DateMatch> parse_date_at(Tokens t, const LocalTime& now, std::size_t p) noexcept {
    if (p >= t.size()) return std::nullopt;
    const std::int64_t today = now.days();
    const SlotToken& k = t[p];

    if (k.kind == TokKind::Word) {
        if (k.text == "today" || k.text == "tonight") return DateMatch{today, p, p + 1};
        if (k.text == "tomorrow") return DateMatch{today + 1, p, p + 1};
        if (k.text == "yesterday") return DateMatch{today - 1, p, p + 1};
        if (k.text == "day" && is_word(t, p + 1, "after") && is_word(t, p + 2, "tomorrow")) return DateMatch{today + 2, p, p + 3};

        // "friday", "this friday", "next friday"
        std::size_t q = p;
        bool next = false;
        if (k.text == "this" || k.text == "next") {
            next = k.text == "next";
            ++q;
        }
        if (q < t.size() && t[q].kind == TokKind::Word) {
            if (const auto wd = lookup(kWeekdays, t[q].text)) {
                int delta = (*wd - static_cast<int>(now.weekday()) + 7) % 7;
                if (delta == 0) delta = 7;
                if (next) delta += 7;
                return DateMatch{today + delta, p, q + 1};
            }
        }

        // "in three days", "in a week", "in two weeks"
        if (k.text == "in") {
            std::size_t r = p + 1;
            double amount = 0;
            if (is_word(t, r, "a") || is_word(t, r, "an")) {
                amount = 1;
                ++r;
            } else if (const auto n = parse_number(t, r); n && is_integer(n->value)) {
                amount = n->value;
                r = n->end;
            } else {
                return std::nullopt;
            }
            if (is_word(t, r, "day") || is_word(t, r, "days")) return DateMatch{today + static_cast<std::int64_t>(amount), p, r + 1};
            if (is_word(t, r, "week") || is_word(t, r, "weeks")) return DateMatch{today + 7 * static_cast<std::int64_t>(amount), p, r + 1};
            return std::nullopt;
        }

        // "may 3rd", "may 3 2027"
        if (const auto month = month_at(t, p)) {
            if (const auto d = parse_day(t, p + 1)) {
                std::size_t end = d->end;
                std::optional<std::int64_t> year;
                if (end < t.size() && is_year(t[end])) {
                    year = static_cast<std::int64_t>(t[end].value);
                    ++end;
                }
                if (const auto day = resolve_month_day(now, *month, d->day, year)) return DateMatch{*day, p, end};
            }
            return std::nullopt;
        }
    }

    // "3rd of may", "3 may", "the third of may 2027"
    std::size_t q = p;
    if (is_word(t, q, "the")) ++q;
    if (const auto d = parse_day(t, q)) {
        std::size_t r = d->end;
        if (is_word(t, r, "of")) ++r;
        if (const auto month = month_at(t, r)) {
            std::size_t end = r + 1;
            std::optional<std::int64_t> year;
            if (end < t.size() && is_year(t[end])) {
                year = static_cast<std::int64_t>(t[end].value);
                ++end;
            }
            if (const auto day = resolve_month_day(now, *month, d->day, year)) return DateMatch{*day, p, end};
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<DateMatch> find_date(Tokens t, const LocalTime& now, std::size_t from) noexcept {
    for (std::size_t p = from; p < t.size(); ++p) {
        if (auto m = parse_date_at(t, now, p)) {
            if (p > from && is_word(t, p - 1, "on")) m->begin = p - 1;
            return m;
        }
    }
    return std::nullopt;
}

}  // namespace assist
