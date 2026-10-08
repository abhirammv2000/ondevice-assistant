#include "assist/calc.hpp"

#include <array>
#include <cmath>
#include <cstdio>

namespace assist {
namespace {

enum class Op : char { Add = '+', Sub = '-', Mul = '*', Div = '/', Pow = '^', PercentOf = '%' };

struct Item {
    bool is_operand;
    double value;  // operand
    Op op;         // operator
};

constexpr std::size_t kMaxItems = 72;  // the lexer keeps at most 64 tokens, and no item is made of less than one

bool is_word(Tokens t, std::size_t i, std::string_view w) noexcept {
    return i < t.size() && t[i].kind == TokKind::Word && t[i].text == w;
}

struct OpMatch {
    Op op;
    std::size_t length;
};

std::optional<OpMatch> match_operator(Tokens t, std::size_t i) noexcept {
    if (is_word(t, i, "plus")) return OpMatch{Op::Add, 1};
    if (is_word(t, i, "minus")) return OpMatch{Op::Sub, 1};
    if (is_word(t, i, "times") || is_word(t, i, "x")) return OpMatch{Op::Mul, 1};
    if ((is_word(t, i, "multiplied") || is_word(t, i, "multiply")) && is_word(t, i + 1, "by")) return OpMatch{Op::Mul, 2};
    if (is_word(t, i, "divided") && is_word(t, i + 1, "by")) return OpMatch{Op::Div, 2};
    if (is_word(t, i, "divide") && is_word(t, i + 1, "by")) return OpMatch{Op::Div, 2};
    if (is_word(t, i, "over")) return OpMatch{Op::Div, 1};
    if (is_word(t, i, "to") && is_word(t, i + 1, "the") && is_word(t, i + 2, "power") && is_word(t, i + 3, "of")) return OpMatch{Op::Pow, 4};
    if (is_word(t, i, "percent") && is_word(t, i + 1, "of")) return OpMatch{Op::PercentOf, 2};
    return std::nullopt;
}

int precedence(Op op) noexcept {
    switch (op) {
        case Op::Pow: return 3;
        case Op::Mul:
        case Op::Div:
        case Op::PercentOf: return 2;
        case Op::Add:
        case Op::Sub: return 1;
    }
    return 0;
}

bool right_associative(Op op) noexcept { return op == Op::Pow; }

}  // namespace

CalcResult evaluate_spoken(Tokens t) noexcept {
    std::array<Item, kMaxItems> items{};
    std::size_t count = 0;
    bool used_function = false;  // a square root, a squared, a cubed or a percent counts as arithmetic by itself
    std::size_t begin = 0;
    std::size_t operand_end = 0;  // one past the last operand (or the postfix word after it) that is part of the expression
    bool started = false;

    std::size_t i = 0;
    while (i < t.size() && count + 2 < kMaxItems) {
        const bool expect_operand = count == 0 || !items[count - 1].is_operand;

        // "negative five", and "minus five" where an operand is expected
        double sign = 1;
        std::size_t j = i;
        if (expect_operand && (is_word(t, j, "negative") || is_word(t, j, "minus"))) {
            sign = -1;
            ++j;
        }
        bool root = false;
        double inner_sign = 1;  // the sign of the number under a square root
        if (expect_operand && is_word(t, j, "square") && is_word(t, j + 1, "root") && is_word(t, j + 2, "of")) {
            root = true;
            j += 3;
            if (is_word(t, j, "negative") || is_word(t, j, "minus")) {
                inner_sign = -1;
                ++j;
            }
        }

        if (expect_operand) {
            if (const auto n = parse_number(t, j)) {
                double value = inner_sign * n->value;
                std::size_t next = n->end;
                if (root) {
                    if (value < 0) return {CalcStatus::NotFinite, 0, i, next};
                    value = std::sqrt(value);
                    used_function = true;
                }
                // postfix words that apply to the number just read
                while (true) {
                    if (is_word(t, next, "squared")) { value *= value; used_function = true; ++next; }
                    else if (is_word(t, next, "cubed")) { value = value * value * value; used_function = true; ++next; }
                    else if (is_word(t, next, "percent") && !is_word(t, next + 1, "of")) { value /= 100; used_function = true; ++next; }
                    else break;
                }
                if (!started) {
                    started = true;
                    begin = i;
                }
                items[count++] = Item{true, sign * value, Op::Add};
                operand_end = next;
                i = next;
                continue;
            }
            if (started) break;  // an operand was expected and is not there, so the expression is over
            // before the expression starts, anything else is filler such as "what is" or "calculate"
            ++i;
            continue;
        }

        // an operator between two operands
        if (const auto m = match_operator(t, i)) {
            items[count++] = Item{false, 0, m->op};
            i += m->length;
            continue;
        }
        break;
    }
    // a trailing operator with no right-hand side is not part of the expression
    while (count > 0 && !items[count - 1].is_operand) {
        --count;
    }
    if (count == 0) return {};
    bool has_binary = false;
    for (std::size_t k = 0; k < count; ++k) has_binary = has_binary || !items[k].is_operand;
    if (!has_binary && !used_function) return {};

    // shunting-yard into a value stack and an operator stack, applying as we go
    std::array<double, kMaxItems> values{};
    std::array<Op, kMaxItems> ops{};
    std::size_t nv = 0, no = 0;
    CalcStatus status = CalcStatus::Ok;

    auto apply = [&](Op op) {
        const double b = values[--nv];
        const double a = values[--nv];
        double r = 0;
        switch (op) {
            case Op::Add: r = a + b; break;
            case Op::Sub: r = a - b; break;
            case Op::Mul: r = a * b; break;
            case Op::Div:
                if (b == 0) { status = CalcStatus::DivideByZero; r = 0; } else { r = a / b; }
                break;
            case Op::Pow: r = std::pow(a, b); break;
            case Op::PercentOf: r = a / 100.0 * b; break;
        }
        values[nv++] = r;
    };

    for (std::size_t k = 0; k < count; ++k) {
        if (items[k].is_operand) {
            values[nv++] = items[k].value;
            continue;
        }
        const Op op = items[k].op;
        while (no > 0 && (precedence(ops[no - 1]) > precedence(op) || (precedence(ops[no - 1]) == precedence(op) && !right_associative(op)))) {
            apply(ops[--no]);
        }
        ops[no++] = op;
    }
    while (no > 0) apply(ops[--no]);

    CalcResult result;
    result.begin = begin;
    result.end = operand_end;
    for (std::size_t k = 0; k < count; ++k) result.operators += items[k].is_operand ? 0U : 1U;
    if (status != CalcStatus::Ok) {
        result.status = status;
        return result;
    }
    const double v = values[0];
    if (!std::isfinite(v) || std::fabs(v) > 1e15) {
        result.status = CalcStatus::NotFinite;
        return result;
    }
    result.status = CalcStatus::Ok;
    result.value = v;
    return result;
}

std::string format_number(double value) {
    if (value == 0) return "0";  // also turns -0 into 0
    // snprintf and not std::to_chars, which needs a newer Apple deployment target for floating point. A host that
    // sets a comma-decimal locale would print "0,3", so the comma is turned back into a point.
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.10g", value);
    for (char* p = buf; *p != '\0'; ++p) {
        if (*p == ',') *p = '.';
    }
    return buf;
}

}  // namespace assist
