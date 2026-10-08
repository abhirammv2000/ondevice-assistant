#include "assist/calendar.hpp"

#include <ctime>

namespace assist {

LocalTime SystemClock::now() const {
    const std::time_t raw = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &raw);
#else
    localtime_r(&raw, &tm);
#endif
    LocalTime t;
    t.year = tm.tm_year + 1900;
    t.month = static_cast<unsigned>(tm.tm_mon + 1);
    t.day = static_cast<unsigned>(tm.tm_mday);
    t.hour = static_cast<unsigned>(tm.tm_hour);
    t.minute = static_cast<unsigned>(tm.tm_min);
    t.second = static_cast<unsigned>(tm.tm_sec > 59 ? 59 : tm.tm_sec);  // a leap second reads as 60
    return t;
}

}  // namespace assist
