#include <cstdint>
#include <string>

#include "assist/redact.hpp"
#include "doctest.h"

using namespace assist;

namespace {

std::string clean(std::string_view text, const Redactor& r = Redactor{}) { return r.redact(text).text; }

}  // namespace

TEST_CASE("luhn: valid and invalid card numbers") {
    CHECK(luhn_valid("4111 1111 1111 1111"));
    CHECK(luhn_valid("4111-1111-1111-1111"));
    CHECK(luhn_valid("5500 0000 0000 0004"));
    CHECK(luhn_valid("378282246310005"));
    CHECK_FALSE(luhn_valid("4111 1111 1111 1112"));
    CHECK_FALSE(luhn_valid("1234 5678 9012 3456"));
    CHECK_FALSE(luhn_valid(""));
    CHECK_FALSE(luhn_valid("4111a111"));
}

TEST_CASE("redact: email addresses") {
    CHECK(clean("send it to jane.roe@example.com please") == "send it to <EMAIL> please");
    CHECK(clean("a+b_c%d@sub.mail-host.co.uk and x@y.io") == "<EMAIL> and <EMAIL>");
    CHECK(clean("mail me at name@example.com.") == "mail me at <EMAIL>.");
    CHECK(Redactor{}.redact("x@y.io").emails == 1);
}

TEST_CASE("redact: things that look like emails but are not") {
    CHECK(clean("meet me @ noon") == "meet me @ noon");
    CHECK(clean("user@localhost") == "user@localhost");
    CHECK(clean("@mention") == "@mention");
    CHECK(clean("a@b.c") == "a@b.c");  // a one-letter top-level domain is not an address
    CHECK(clean("me@") == "me@");
}

TEST_CASE("redact: phone numbers in the usual shapes") {
    CHECK(clean("call 415-555-2671 now") == "call <PHONE> now");
    CHECK(clean("call (415) 555-2671 now") == "call <PHONE> now");
    CHECK(clean("call +1 415 555 2671 now") == "call <PHONE> now");
    CHECK(clean("call 4155552671") == "call <PHONE>");
    CHECK(clean("call 415.555.2671") == "call <PHONE>");
    CHECK(clean("call 555-1234") == "call <PHONE>");
    CHECK(clean("+44 20 7946 0958") == "<PHONE>");
}

TEST_CASE("redact: payment cards need the Luhn check, social security numbers a fixed shape") {
    CHECK(clean("my card is 4111 1111 1111 1111 thanks") == "my card is <CARD> thanks");
    CHECK(clean("4111-1111-1111-1111") == "<CARD>");
    CHECK(clean("4111111111111111") == "<CARD>");
    CHECK(clean("order 1234 5678 9012 3456") == "order 1234 5678 9012 3456");  // sixteen digits, not a card
    CHECK(clean("my ssn is 123-45-6789") == "my ssn is <SSN>");
}

TEST_CASE("redact: dates, times and short numbers are left alone, because the answer needs them") {
    for (const char* text : {"remind me on 2026-10-08", "set an alarm for 7:30 pm", "a timer for 10 minutes", "call 911", "room 1204", "on 08-10-2026", "year 2027",
                              "at 2026-10-08 09:15:00 sharp", "12345", "1234567", "3.14159"}) {
        CHECK_MESSAGE(clean(text) == text, text);
    }
}

TEST_CASE("redact: digits inside a word are not a number") {
    CHECK(clean("order abc4155552671") == "order abc4155552671");
    CHECK(clean("x4111111111111111") == "x4111111111111111");
}

TEST_CASE("redact: digits spoken one at a time") {
    CHECK(clean("my number is five five five one two three four") == "my number is <PHONE>");
    CHECK(clean("call Four One Five five five five two six seven one") == "call <PHONE>");
    CHECK(clean("it is oh one two three four five six") == "it is <PHONE>");
    CHECK(clean("card four one one one one one one one one one one one one one one one") == "card <CARD>");
    CHECK(clean("one two three four five six") == "one two three four five six");  // six words, too short
    CHECK(clean("one two three four five six seven eight nine ten") == "<PHONE> ten");
}

TEST_CASE("redact: contact names, whole words only, in any case") {
    Redactor r;
    r.add_name("Sarah Connor");
    r.add_name("Mom");
    r.add_name("Al");  // under three letters, so it is not added
    CHECK(clean("text sarah that i am late", r) == "text <NAME> that i am late");
    CHECK(clean("call MOM and CONNOR", r) == "call <NAME> and <NAME>");
    CHECK(clean("sara and sarahs and momentum", r) == "sara and sarahs and momentum");
    CHECK(clean("al is here", r) == "al is here");
    CHECK(r.redact("sarah connor and mom").names == 3);
}

TEST_CASE("redact: everything at once, with the counts") {
    Redactor r;
    r.add_name("Dana");
    const auto out = r.redact("tell dana to email dana@work.org or call 415-555-2671, card 4111 1111 1111 1111, ssn 123-45-6789");
    CHECK(out.text == "tell <NAME> to email <EMAIL> or call <PHONE>, card <CARD>, ssn <SSN>");
    CHECK(out.names == 1);
    CHECK(out.emails == 1);
    CHECK(out.phones == 1);
    CHECK(out.cards == 1);
    CHECK(out.ssns == 1);
    CHECK(out.total() == 5);
}

TEST_CASE("redact: running it again changes nothing, and a placeholder is not mistaken for a name") {
    Redactor r;
    r.add_name("Phone");
    r.add_name("Email");
    const std::string once = clean("call 415-555-2671 or mail a@b.com about the phone", r);
    CHECK(once == "call <PHONE> or mail <EMAIL> about the <NAME>");
    CHECK(clean(once, r) == once);
}

TEST_CASE("redact: hostile input does not crash and leaves no email address behind") {
    std::uint32_t state = 5;
    auto next = [&] {
        state = state * 1664525U + 1013904223U;
        return state >> 8;
    };
    const char alphabet[] = "ab@.0123456789 -+()xyz\xC3\xA9";
    Redactor r;
    r.add_name("abc");
    for (int i = 0; i < 4000; ++i) {
        std::string s;
        for (unsigned n = next() % 60; n > 0; --n) s.push_back(alphabet[next() % (sizeof alphabet - 1)]);
        const auto once = r.redact(s);
        REQUIRE(r.redact(once.text).text == once.text);  // idempotent
    }
    CHECK(clean("@@@@....@@..") == "@@@@....@@..");
    CHECK(clean(std::string(5000, '9')) == std::string(5000, '9'));  // far too long to be a phone number or a card
}
