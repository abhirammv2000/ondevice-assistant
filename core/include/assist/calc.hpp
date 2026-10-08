// Spoken arithmetic: "what is twelve times seven plus three", "fifteen percent of eighty", "square root of eighty one".
//
// The speech recogniser hands over words, so the operators are words too: plus, minus, times, multiplied by,
// divided by, over, "to the power of", "percent of", squared, cubed, "square root of", and "negative" for a sign.
// Precedence is the usual one: powers, then times and divide and percent-of, then plus and minus, left to right
// (powers from the right). There are no brackets, since nobody says them aloud.
#pragma once

#include <cstddef>
#include <string>

#include "assist/slots.hpp"

namespace assist {

enum class CalcStatus {
    Ok,
    NoExpression,   // no arithmetic found, for example "what is five"
    DivideByZero,
    NotFinite,      // square root of a negative number, or a result too large to be useful
};

struct CalcResult {
    CalcStatus status = CalcStatus::NoExpression;
    double value = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
    unsigned operators = 0;  // how many plus, minus, times, divide, power or percent-of signs were evaluated
};

// Find and evaluate the first arithmetic expression in the tokens.
CalcResult evaluate_spoken(Tokens t) noexcept;

// 10 significant digits, no trailing zeros, no exponent for ordinary sizes: 84, 0.3, 12.5, 1000000.
std::string format_number(double value);

}  // namespace assist
