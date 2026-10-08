// Removing personal data from an utterance before it leaves the device.
//
// The request is first tried on the device. When it has to go to a bigger model elsewhere, what is sent is the
// redacted text: email addresses, phone numbers, payment card numbers (checked with the Luhn formula so a long
// number that is not a card is not mistaken for one), social security numbers, digits spoken one by one ("five five
// five one two three four"), and the names of the user's own contacts. Dates, times and short numbers are left alone
// because the larger model needs them to answer.
//
// This is a pattern matcher, not a promise. A name it was not told about, or an address written out in words, passes
// through. docs/DESIGN.md says what it does not catch.
#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

namespace assist {

struct RedactResult {
    std::string text;
    unsigned emails = 0;
    unsigned phones = 0;
    unsigned cards = 0;
    unsigned ssns = 0;
    unsigned names = 0;

    unsigned total() const noexcept { return emails + phones + cards + ssns + names; }
};

class Redactor {
public:
    // A name from the user's contacts. Each word of three or more letters is replaced wherever it appears as a
    // whole word, in any case.
    void add_name(std::string_view name);

    RedactResult redact(std::string_view text) const;

private:
    std::unordered_set<std::string> name_words_;
};

// The Luhn check over the digits of a string, ignoring spaces and dashes. Exposed for the tests.
bool luhn_valid(std::string_view digits_with_separators) noexcept;

}  // namespace assist
