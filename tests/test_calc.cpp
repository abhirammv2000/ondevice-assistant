#include <cmath>
#include <string>

#include "assist/calc.hpp"
#include "doctest.h"

using namespace assist;

namespace {

struct Lexed {
    SlotText text;
    explicit Lexed(std::string_view s) { text.lex(s); }
};

CalcResult calc(std::string_view s) {
    Lexed l(s);
    return evaluate_spoken(l.text.tokens());
}

}  // namespace

TEST_CASE("calc: the four operations in digits and in words") {
    struct Case {
        const char* text;
        double value;
    };
    const Case cases[] = {
        {"12 times 7", 84},
        {"twelve times seven", 84},
        {"5 plus 6", 11},
        {"five plus six", 11},
        {"10 minus 4", 6},
        {"4 minus 10", -6},
        {"10 divided by 4", 2.5},
        {"ten over four", 2.5},
        {"3 multiplied by 4", 12},
        {"3 x 4", 12},
        {"twenty five plus seventy five", 100},
        {"one hundred and five minus five", 100},
        {"two thousand twenty six minus one thousand", 1026},
    };
    for (const auto& c : cases) {
        const auto r = calc(c.text);
        REQUIRE_MESSAGE(r.status == CalcStatus::Ok, c.text);
        CHECK_MESSAGE(r.value == doctest::Approx(c.value), c.text);
    }
}

TEST_CASE("calc: precedence and associativity") {
    CHECK(calc("2 plus 3 times 4").value == 14);  // times first
    CHECK(calc("2 times 3 plus 4").value == 10);
    CHECK(calc("10 minus 4 minus 3").value == 3);  // left to right
    CHECK(calc("100 divided by 10 divided by 2").value == 5);
    CHECK(calc("2 to the power of 3 to the power of 2").value == 512);  // powers from the right
    CHECK(calc("2 times 3 to the power of 2").value == 18);
    CHECK(calc("one plus two times three plus four").value == 11);
}

TEST_CASE("calc: percent, powers, squares and roots") {
    CHECK(calc("what is fifteen percent of eighty").value == doctest::Approx(12));
    CHECK(calc("15 percent of 80").value == doctest::Approx(12));
    CHECK(calc("50 percent").value == doctest::Approx(0.5));
    CHECK(calc("2 to the power of 8").value == 256);
    CHECK(calc("5 squared").value == 25);
    CHECK(calc("3 cubed plus 1").value == 28);
    CHECK(calc("square root of 81").value == 9);
    CHECK(calc("what is the square root of 2 times 2").value == doctest::Approx(2.8284271247));
    CHECK(calc("10 plus 5 percent of 200").value == doctest::Approx(20));
}

TEST_CASE("calc: signs") {
    CHECK(calc("negative five plus ten").value == 5);
    CHECK(calc("minus 5 plus 10").value == 5);
    CHECK(calc("5 times negative 3").value == -15);
    CHECK(calc("ten minus negative three").value == 13);
}

TEST_CASE("calc: found inside a sentence, with the range of the expression") {
    const auto r = calc("hey what is twelve times seven please");
    REQUIRE(r.status == CalcStatus::Ok);
    CHECK(r.value == 84);
    CHECK(r.begin == 3);
    CHECK(r.end == 6);
}

TEST_CASE("calc: not arithmetic") {
    for (const char* text : {"", "what is five", "set a timer for 10 minutes", "hello", "plus", "times times", "what time is it"}) {
        CHECK_MESSAGE(calc(text).status == CalcStatus::NoExpression, text);
    }
}

TEST_CASE("calc: a trailing operator is left out of the expression") {
    const auto r = calc("5 plus 3 times");
    REQUIRE(r.status == CalcStatus::Ok);
    CHECK(r.value == 8);
    CHECK(r.end == 3);
}

TEST_CASE("calc: errors are reported, not computed") {
    CHECK(calc("5 divided by 0").status == CalcStatus::DivideByZero);
    CHECK(calc("5 divided by zero").status == CalcStatus::DivideByZero);
    CHECK(calc("square root of negative nine").status == CalcStatus::NotFinite);
    CHECK(calc("1000000 times 1000000 times 1000000").status == CalcStatus::NotFinite);
}

TEST_CASE("calc: a very long expression stops at the buffer limit instead of overflowing") {
    std::string text = "1";
    for (int i = 0; i < 100; ++i) text += " plus 1";
    const auto r = calc(text);
    // the lexer keeps 64 tokens: 32 ones and 32 pluses, and the last plus has nothing after it so it is dropped
    REQUIRE(r.status == CalcStatus::Ok);
    CHECK(r.value == 32);
}

TEST_CASE("calc: random sums of two numbers match direct arithmetic") {
    std::uint32_t state = 4242;
    auto next = [&] {
        state = state * 1664525U + 1013904223U;
        return state >> 8;
    };
    const char* names[] = {"plus", "minus", "times", "divided by"};
    for (int i = 0; i < 3000; ++i) {
        const int a = static_cast<int>(next() % 1000);
        const int b = static_cast<int>(next() % 999) + 1;
        const int op = static_cast<int>(next() % 4);
        const std::string text = std::to_string(a) + " " + names[op] + " " + std::to_string(b);
        const double want = op == 0 ? a + b : op == 1 ? a - b : op == 2 ? static_cast<double>(a) * b : static_cast<double>(a) / b;
        const auto r = calc(text);
        REQUIRE_MESSAGE(r.status == CalcStatus::Ok, text);
        REQUIRE_MESSAGE(std::fabs(r.value - want) < 1e-9, text);
    }
}

TEST_CASE("format_number: ten significant digits and no clutter") {
    CHECK(format_number(84) == "84");
    CHECK(format_number(0.3) == "0.3");
    CHECK(format_number(0.1 + 0.2) == "0.3");
    CHECK(format_number(12.5) == "12.5");
    CHECK(format_number(1000000) == "1000000");
    CHECK(format_number(-6) == "-6");
    CHECK(format_number(0) == "0");
    CHECK(format_number(-0.0) == "0");
    CHECK(format_number(1.0 / 3.0) == "0.3333333333");
    CHECK(format_number(2.8284271247461903) == "2.828427125");
}
