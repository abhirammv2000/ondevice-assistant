#include "assist/redact.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace assist {
namespace {

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool is_word_byte(char c) noexcept { return is_alpha(c) || is_digit(c) || c == '\'' || static_cast<unsigned char>(c) >= 0x80; }
constexpr char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

// emails

constexpr bool local_char(char c) noexcept { return is_alpha(c) || is_digit(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-'; }
constexpr bool domain_char(char c) noexcept { return is_alpha(c) || is_digit(c) || c == '.' || c == '-'; }

std::string redact_emails(std::string_view in, unsigned& count) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        if (in[i] != '@') {
            out.push_back(in[i++]);
            continue;
        }
        // grow left over what was already copied, and right over the domain
        std::size_t left = out.size();
        while (left > 0 && local_char(out[left - 1])) --left;
        std::size_t right = i + 1;
        while (right < in.size() && domain_char(in[right])) ++right;
        // the domain must contain a dot followed by at least two letters, and not end in a dot
        std::size_t end = right;
        while (end > i + 1 && (in[end - 1] == '.' || in[end - 1] == '-')) --end;
        const std::string_view domain = in.substr(i + 1, end - (i + 1));
        const std::size_t dot = domain.rfind('.');
        const bool ok = left < out.size() && dot != std::string_view::npos && dot > 0 && domain.size() - dot - 1 >= 2 &&
                        std::all_of(domain.begin() + static_cast<std::ptrdiff_t>(dot) + 1, domain.end(), is_alpha);
        if (!ok) {
            out.push_back(in[i++]);
            continue;
        }
        out.resize(left);
        out += "<EMAIL>";
        ++count;
        i = end;
    }
    return out;
}

// numbers

struct Run {
    std::size_t begin;
    std::size_t end;           // one past the last digit
    std::vector<int> groups;   // digits in each group between separators
    int digits;
    bool only_space_dash;      // separators were all spaces or dashes
    bool dashes_only;
};

bool is_separator(char c) noexcept { return c == ' ' || c == '-' || c == '.' || c == '(' || c == ')'; }

// Find the digit run that starts at i, which holds a digit, a '+' or a '('.
Run read_run(std::string_view s, std::size_t i) {
    Run r{i, i, {}, 0, true, true};
    std::size_t j = i;
    if (j < s.size() && s[j] == '+') ++j;
    int group = 0;
    std::size_t last_digit_end = i;
    while (j < s.size()) {
        const char c = s[j];
        if (is_digit(c)) {
            ++group;
            ++r.digits;
            ++j;
            last_digit_end = j;
        } else if (is_separator(c) && group > 0 && j + 1 < s.size() && (is_digit(s[j + 1]) || is_separator(s[j + 1]) || s[j + 1] == '(')) {
            // one separator, or ") " / "- " pairs, between digit groups
            r.groups.push_back(group);
            group = 0;
            if (c != ' ' && c != '-') r.only_space_dash = false;
            if (c != '-') r.dashes_only = false;
            ++j;
        } else if (is_separator(c) && group == 0 && (c == '(' || c == ' ')) {
            if (c != ' ' && c != '-') r.only_space_dash = false;
            r.dashes_only = false;
            ++j;
        } else {
            break;
        }
    }
    if (group > 0) r.groups.push_back(group);
    r.end = last_digit_end;
    return r;
}

// 2026-10-08 or 08-10-2026, optionally followed by a time such as 09 15 00, which is not a phone number
bool is_date_shape(const std::vector<int>& g) {
    if (g.size() < 3 || g.size() > 6) return false;
    const bool date = (g[0] == 4 && g[1] <= 2 && g[2] <= 2) || (g[0] <= 2 && g[1] <= 2 && g[2] == 4);
    return date && std::all_of(g.begin() + 3, g.end(), [](int n) { return n <= 2; });
}

}  // namespace

bool luhn_valid(std::string_view s) noexcept {
    int sum = 0;
    bool doubled = false;
    int digits = 0;
    for (std::size_t i = s.size(); i > 0; --i) {
        const char c = s[i - 1];
        if (c == ' ' || c == '-') continue;
        if (!is_digit(c)) return false;
        int d = c - '0';
        if (doubled) {
            d *= 2;
            if (d > 9) d -= 9;
        }
        sum += d;
        doubled = !doubled;
        ++digits;
    }
    return digits > 0 && sum % 10 == 0;
}

namespace {

std::string redact_numbers(std::string_view in, RedactResult& r) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const char c = in[i];
        if (is_digit(c) && i > 0 && is_alpha(in[i - 1])) {  // digits that are part of a word, such as "abc123", are kept whole
            while (i < in.size() && is_digit(in[i])) out.push_back(in[i++]);
            continue;
        }
        const bool starts = is_digit(c) || ((c == '+' || c == '(') && i + 1 < in.size() && (is_digit(in[i + 1]) || in[i + 1] == '('));
        // a digit in the middle of a word, such as "abc123", is not the start of a number
        if (!starts || (i > 0 && is_alpha(in[i - 1]))) {
            out.push_back(in[i++]);
            continue;
        }
        const Run run = read_run(in, i);
        const std::string_view span = in.substr(run.begin, run.end - run.begin);
        const bool ssn = run.groups == std::vector<int>{3, 2, 4} && run.dashes_only;
        const bool card = run.digits >= 13 && run.digits <= 19 && run.only_space_dash && luhn_valid(span);
        const bool phone = !is_date_shape(run.groups) && ((run.digits >= 10 && run.digits <= 15) || (run.groups == std::vector<int>{3, 4} && run.dashes_only));
        if (ssn) {
            out += "<SSN>";
            ++r.ssns;
        } else if (card) {
            out += "<CARD>";
            ++r.cards;
        } else if (phone) {
            out += "<PHONE>";
            ++r.phones;
        } else {
            out.append(in.substr(i, std::max<std::size_t>(run.end, i + 1) - i));
            i = std::max<std::size_t>(run.end, i + 1);
            continue;
        }
        i = run.end;
    }
    return out;
}

// digits spoken one at a time

int digit_word(std::string_view w) noexcept {
    static constexpr std::array<std::string_view, 11> words{"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "oh"};
    for (std::size_t k = 0; k < words.size(); ++k) {
        if (w.size() == words[k].size() && std::equal(w.begin(), w.end(), words[k].begin(), [](char a, char b) { return lower(a) == b; })) return static_cast<int>(k);
    }
    return -1;
}

std::string redact_spoken_digits(std::string_view in, RedactResult& r) {
    struct Word {
        std::size_t begin, end;
        bool digit;
    };
    std::vector<Word> words;
    for (std::size_t i = 0; i < in.size();) {
        if (!is_word_byte(in[i])) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < in.size() && is_word_byte(in[j])) ++j;
        words.push_back({i, j, digit_word(in.substr(i, j - i)) >= 0});
        i = j;
    }
    std::string out;
    std::size_t copied = 0;
    for (std::size_t k = 0; k < words.size();) {
        if (!words[k].digit) {
            ++k;
            continue;
        }
        std::size_t m = k;
        while (m + 1 < words.size() && words[m + 1].digit) ++m;
        const std::size_t count = m - k + 1;
        if (count >= 7) {
            out.append(in.substr(copied, words[k].begin - copied));
            if (count >= 13) {
                out += "<CARD>";
                ++r.cards;
            } else {
                out += "<PHONE>";
                ++r.phones;
            }
            copied = words[m].end;
        }
        k = m + 1;
    }
    out.append(in.substr(copied));
    return out;
}

}  // namespace

void Redactor::add_name(std::string_view name) {
    std::size_t i = 0;
    while (i < name.size()) {
        while (i < name.size() && !is_word_byte(name[i])) ++i;
        std::string word;
        while (i < name.size() && is_word_byte(name[i])) word.push_back(lower(name[i++]));
        if (word.size() >= 3) name_words_.insert(std::move(word));
    }
}

RedactResult Redactor::redact(std::string_view text) const {
    RedactResult r;
    std::string s = redact_emails(text, r.emails);
    s = redact_numbers(s, r);
    s = redact_spoken_digits(s, r);
    if (name_words_.empty()) {
        r.text = std::move(s);
        return r;
    }
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (!is_word_byte(s[i])) {
            out.push_back(s[i++]);
            continue;
        }
        std::size_t j = i;
        std::string lowered;
        while (j < s.size() && is_word_byte(s[j])) lowered.push_back(lower(s[j++]));
        const bool placeholder = i > 0 && s[i - 1] == '<' && j < s.size() && s[j] == '>';  // an earlier replacement
        if (!placeholder && name_words_.count(lowered) != 0) {
            out += "<NAME>";
            ++r.names;
        } else {
            out.append(s, i, j - i);
        }
        i = j;
    }
    r.text = std::move(out);
    return r;
}

}  // namespace assist
