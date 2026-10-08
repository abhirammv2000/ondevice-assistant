// Calendar arithmetic and the clock the rest of the code reads the time from.
//
// Dates are converted with the civil-from-days algorithms from Howard Hinnant's "chrono-Compatible Low-Level Date
// Algorithms" (public domain). They work on whole days from 1970-01-01 with integer maths only, are valid over
// the whole proleptic Gregorian calendar, and tests/ checks them against Python's datetime for every day from
// 1900 to 2300.
//
// Times here are wall-clock times: a date and a time of day as a person would read them, with no time zone or
// daylight-saving rule attached. The host owns the zone, so "alarm at 7 am" is resolved against the host's own
// reading of "now" and stays correct across a clock change without this code knowing the rules.
#pragma once

#include <cstdint>

namespace assist {

constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
}

// 0 is Sunday, 6 is Saturday. 1970-01-01 was a Thursday.
constexpr unsigned weekday_from_days(std::int64_t z) noexcept {
    return static_cast<unsigned>(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

constexpr bool is_leap_year(std::int64_t y) noexcept { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }

constexpr unsigned days_in_month(std::int64_t y, unsigned m) noexcept {
    constexpr unsigned table[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && is_leap_year(y)) ? 29U : table[m - 1];
}

struct LocalTime {
    std::int64_t year = 1970;
    unsigned month = 1;   // 1 to 12
    unsigned day = 1;     // 1 to 31
    unsigned hour = 0;    // 0 to 23
    unsigned minute = 0;  // 0 to 59
    unsigned second = 0;  // 0 to 59

    friend constexpr bool operator==(const LocalTime&, const LocalTime&) = default;

    constexpr std::int64_t days() const noexcept { return days_from_civil(year, month, day); }
    constexpr unsigned weekday() const noexcept { return weekday_from_days(days()); }
    // Seconds since 1970-01-01 00:00:00 on the wall clock, which is only meaningful for comparing and adding.
    constexpr std::int64_t seconds() const noexcept { return days() * 86400 + hour * 3600 + minute * 60 + second; }

    static constexpr LocalTime from_seconds(std::int64_t s) noexcept {
        std::int64_t days = s / 86400;
        std::int64_t rem = s % 86400;
        if (rem < 0) {
            rem += 86400;
            --days;
        }
        LocalTime t;
        civil_from_days(days, t.year, t.month, t.day);
        t.hour = static_cast<unsigned>(rem / 3600);
        t.minute = static_cast<unsigned>(rem % 3600 / 60);
        t.second = static_cast<unsigned>(rem % 60);
        return t;
    }
};

// The only way the rest of the code learns what time it is. Tests give it a FixedClock, the host gives it the system's.
class Clock {
public:
    virtual ~Clock() = default;
    virtual LocalTime now() const = 0;
};

class FixedClock final : public Clock {
public:
    explicit FixedClock(LocalTime t) : t_(t) {}
    LocalTime now() const override { return t_; }
    void set(LocalTime t) { t_ = t; }

private:
    LocalTime t_;
};

class SystemClock final : public Clock {
public:
    LocalTime now() const override;  // the host's local time, read with localtime_r or localtime_s
};

}  // namespace assist
