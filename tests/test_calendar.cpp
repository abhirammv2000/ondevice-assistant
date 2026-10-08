#include <fstream>
#include <sstream>
#include <string>

#include "assist/calendar.hpp"
#include "doctest.h"

#ifndef ASSIST_GOLDEN_DIR
#define ASSIST_GOLDEN_DIR "tests/golden"
#endif

using namespace assist;

TEST_CASE("calendar: known anchors") {
    CHECK(days_from_civil(1970, 1, 1) == 0);
    CHECK(days_from_civil(1970, 1, 2) == 1);
    CHECK(days_from_civil(1969, 12, 31) == -1);
    CHECK(days_from_civil(2000, 1, 1) == 10957);
    CHECK(days_from_civil(2000, 3, 1) - days_from_civil(2000, 2, 28) == 2);  // 2000 is a leap year
    CHECK(days_from_civil(1900, 3, 1) - days_from_civil(1900, 2, 28) == 1);  // 1900 is not
    CHECK(weekday_from_days(0) == 4);                                         // Thursday
    CHECK(weekday_from_days(-1) == 3);
    CHECK(weekday_from_days(-4) == 0);
    CHECK(weekday_from_days(-5) == 6);
}

TEST_CASE("calendar: leap years and month lengths") {
    CHECK(is_leap_year(2000));
    CHECK_FALSE(is_leap_year(1900));
    CHECK(is_leap_year(2024));
    CHECK_FALSE(is_leap_year(2023));
    CHECK(days_in_month(2024, 2) == 29);
    CHECK(days_in_month(2023, 2) == 28);
    CHECK(days_in_month(2023, 12) == 31);
    CHECK(days_in_month(2023, 4) == 30);
}

TEST_CASE("calendar: every day from 1900 to 2300 round-trips and the days follow each other correctly") {
    const std::int64_t first = days_from_civil(1900, 1, 1);
    const std::int64_t last = days_from_civil(2300, 12, 31);
    std::int64_t py = 0;
    unsigned pm = 0, pd = 0;
    unsigned previous_weekday = 0;
    for (std::int64_t z = first; z <= last; ++z) {
        std::int64_t y;
        unsigned m, d;
        civil_from_days(z, y, m, d);
        REQUIRE(days_from_civil(y, m, d) == z);
        REQUIRE(m >= 1);
        REQUIRE(m <= 12);
        REQUIRE(d >= 1);
        REQUIRE(d <= days_in_month(y, m));
        const unsigned wd = weekday_from_days(z);
        if (z > first) {
            // the next day is either the next day of the month, or the 1st of the next month or year
            if (pd < days_in_month(py, pm)) {
                REQUIRE(y == py);
                REQUIRE(m == pm);
                REQUIRE(d == pd + 1);
            } else {
                REQUIRE(d == 1);
                REQUIRE(((pm == 12 && m == 1 && y == py + 1) || (m == pm + 1 && y == py)));
            }
            REQUIRE(wd == (previous_weekday + 1) % 7);
        }
        py = y;
        pm = m;
        pd = d;
        previous_weekday = wd;
    }
}

TEST_CASE("calendar: the C++ maths equals Python's datetime on the golden dates") {
    std::ifstream in(std::string(ASSIST_GOLDEN_DIR) + "/calendar.tsv");
    REQUIRE_MESSAGE(in.good(), "run python tools/make_golden.py first");
    std::int64_t y;
    unsigned m, d, wd;
    std::int64_t days;
    int checked = 0;
    while (in >> y >> m >> d >> days >> wd) {
        CHECK_MESSAGE(days_from_civil(y, m, d) == days, y << "-" << m << "-" << d);
        CHECK_MESSAGE(weekday_from_days(days) == wd, y << "-" << m << "-" << d);
        std::int64_t cy;
        unsigned cm, cd;
        civil_from_days(days, cy, cm, cd);
        CHECK((cy == y && cm == m && cd == d));
        ++checked;
    }
    CHECK(checked > 3000);
}

TEST_CASE("LocalTime: converts to and from seconds, including before 1970") {
    const LocalTime t{2026, 10, 8, 9, 15, 30};
    CHECK(LocalTime::from_seconds(t.seconds()) == t);
    CHECK(t.weekday() == 4);  // 2026-10-08 is a Thursday

    const LocalTime epoch{1970, 1, 1, 0, 0, 0};
    CHECK(epoch.seconds() == 0);
    CHECK(LocalTime::from_seconds(0) == epoch);

    const LocalTime before = LocalTime::from_seconds(-1);
    CHECK(before == LocalTime{1969, 12, 31, 23, 59, 59});
    CHECK(LocalTime::from_seconds(-86400 * 366 - 3600) == LocalTime{1968, 12, 30, 23, 0, 0});

    const LocalTime year_end{2026, 12, 31, 23, 59, 59};
    CHECK(LocalTime::from_seconds(year_end.seconds() + 1) == LocalTime{2027, 1, 1, 0, 0, 0});
}

TEST_CASE("clocks: a fixed clock is fixed and can be moved, the system clock reads something plausible") {
    FixedClock fixed(LocalTime{2026, 10, 8, 9, 15, 0});
    CHECK(fixed.now() == LocalTime{2026, 10, 8, 9, 15, 0});
    fixed.set(LocalTime{2027, 1, 1, 0, 0, 0});
    CHECK(fixed.now().year == 2027);

    const LocalTime now = SystemClock().now();
    CHECK(now.year >= 2025);
    CHECK(now.month >= 1);
    CHECK(now.month <= 12);
    CHECK(now.day >= 1);
    CHECK(now.day <= 31);
    CHECK(now.hour <= 23);
    CHECK(now.minute <= 59);
    CHECK(now.second <= 59);
}
