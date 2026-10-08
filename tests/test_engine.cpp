#include <memory>
#include <string>
#include <vector>

#include "assist/engine.hpp"
#include "doctest.h"
#include "model_builder.hpp"

using namespace assist;

namespace {

// 2026-10-08 is a Thursday, 09:15
const LocalTime kNow{2026, 10, 8, 9, 15, 0};

Model stub_model() {
    const std::vector<std::string> classes{"timer", "alarm", "time", "date", "calculator", "flip_coin", "roll_dice", "make_call",
                                           "text", "reminder_update", "greeting", "goodbye", "thank_you", "weather", "other"};
    const std::vector<std::pair<std::string, std::string>> triggers{
        {"timer", "timer"}, {"wake", "alarm"}, {"alarm", "alarm"}, {"time", "time"}, {"date", "date"}, {"calculate", "calculator"},
        {"coin", "flip_coin"}, {"flip", "flip_coin"}, {"dice", "roll_dice"}, {"die", "roll_dice"}, {"roll", "roll_dice"},
        {"call", "make_call"}, {"text", "text"}, {"remind", "reminder_update"}, {"hello", "greeting"}, {"goodbye", "goodbye"},
        {"thanks", "thank_you"}, {"weather", "weather"}};
    const auto bytes = testutil::build_trigger_model(classes, triggers);
    Model m;
    REQUIRE(Model::from_bytes(bytes, m) == LoadStatus::Ok);
    return m;
}

struct Fixture {
    std::shared_ptr<FixedClock> clock = std::make_shared<FixedClock>(kNow);
    Engine engine;

    explicit Fixture(EngineConfig config = [] {
        EngineConfig c;
        c.random_seed = 12345;
        return c;
    }())
        : engine(stub_model(), clock, config) {
        engine.set_contacts({{1, "Mom"}, {2, "John Smith"}, {3, "Jon Snow"}, {4, "Sarah Connor"}, {5, "Sara Conner"}, {6, "Dana"}});
    }

    Result ask(std::string_view text) { return engine.handle(text); }
};

}  // namespace

// timers

TEST_CASE("timer: a duration sets it, and the reply says it back") {
    Fixture f;
    const Result r = f.ask("set a timer for ten minutes");
    CHECK(r.route == Route::OnDevice);
    CHECK(r.intent == "timer");
    CHECK(r.action == "timer.set");
    CHECK(*r.slot("seconds") == "600");
    CHECK(r.reply == "Timer set for 10 minutes.");

    const Result long_one = f.ask("set a timer for an hour and a half");
    CHECK(*long_one.slot("seconds") == "5400");
    CHECK(long_one.reply == "Timer set for 1 hour 30 minutes.");
    CHECK(f.ask("timer for 1 minute").reply == "Timer set for 1 minute.");
}

TEST_CASE("timer: with no duration the device asks for one") {
    Fixture f;
    const Result r = f.ask("please set a timer for me");
    CHECK(r.route == Route::Clarify);
    CHECK(r.reason == "missing_duration");
    CHECK(r.reply == "For how long?");
    CHECK(r.action == "timer.set");
}

// alarms

TEST_CASE("alarm: times resolve against the clock") {
    Fixture f;
    auto at = [&](const char* text) { return f.ask(text); };
    CHECK(*at("wake me at 7:30 tomorrow").slot("at") == "2026-10-09T07:30");
    CHECK(at("wake me at 7:30 tomorrow").reply == "Alarm set for tomorrow at 7:30 AM.");
    CHECK(*at("wake me at 6 am").slot("at") == "2026-10-09T06:00");       // 6 am has passed today
    CHECK(*at("wake me at 7").slot("at") == "2026-10-08T19:00");           // 7 am has passed, so 7 pm
    CHECK(*at("wake me at 10").slot("at") == "2026-10-08T10:00");          // 10 am is still ahead
    CHECK(*at("wake me at 8 tonight").slot("at") == "2026-10-08T20:00");
    CHECK(*at("wake me friday at 6:30 am").slot("at") == "2026-10-09T06:30");
    CHECK(at("wake me friday at 6:30 am").reply == "Alarm set for tomorrow at 6:30 AM.");
    CHECK(at("wake me monday at 6:30 am").reply == "Alarm set for Monday, October 12, 2026 at 6:30 AM.");
    CHECK(*at("wake me in 20 minutes").slot("at") == "2026-10-08T09:35");
}

TEST_CASE("alarm: a missing or past time is asked about, not guessed") {
    Fixture f;
    const Result none = f.ask("wake me up");
    CHECK(none.route == Route::Clarify);
    CHECK(none.reason == "missing_time");

    const Result past = f.ask("wake me at 7 am today");
    CHECK(past.route == Route::Clarify);
    CHECK(past.reason == "time_passed");
}

// time and date

TEST_CASE("time and date come from the clock") {
    Fixture f;
    CHECK(f.ask("what time is it").reply == "It is 9:15 AM.");
    f.clock->set(LocalTime{2026, 10, 8, 0, 5, 0});
    CHECK(f.ask("what time is it").reply == "It is 12:05 AM.");
    f.clock->set(LocalTime{2026, 10, 8, 12, 0, 0});
    CHECK(f.ask("what time is it").reply == "It is 12:00 PM.");
    f.clock->set(kNow);

    CHECK(f.ask("what is the date").reply == "Today is Thursday, October 8, 2026.");
    CHECK(f.ask("what is the date tomorrow").reply == "Tomorrow is Friday, October 9, 2026.");
    CHECK(f.ask("what is the date yesterday").reply == "Yesterday was Wednesday, October 7, 2026.");
    CHECK(f.ask("what is the date next friday").reply == "That is Friday, October 16, 2026.");
}

TEST_CASE("time in another place needs data the device does not have, so it goes elsewhere") {
    Fixture f;
    const Result r = f.ask("what time is it in tokyo");
    CHECK(r.route == Route::Escalate);
    CHECK(r.reason == "needs_timezone_data");
    CHECK(r.forward_text == "what time is it in tokyo");
}

// calculator, coin, dice

TEST_CASE("calculator: answers, refuses to divide by zero, and sends on what it cannot work out") {
    Fixture f;
    CHECK(f.ask("calculate twelve times seven").reply == "84.");
    CHECK(f.ask("calculate ten divided by four").reply == "2.5.");
    CHECK(f.ask("calculate five divided by zero").reply == "I can't divide by zero.");
    const Result r = f.ask("calculate banana");
    CHECK(r.route == Route::Escalate);
    CHECK(r.reason == "no_arithmetic_found");
}

TEST_CASE("coin and dice are random but repeatable from a seed, and in range") {
    Fixture a, b;
    for (int i = 0; i < 20; ++i) {
        CHECK(a.ask("flip a coin").reply == b.ask("flip a coin").reply);
    }
    int heads = 0;
    for (int i = 0; i < 200; ++i) heads += *a.ask("flip a coin").slot("result") == "heads";
    CHECK(heads > 60);
    CHECK(heads < 140);

    for (int i = 0; i < 100; ++i) {
        const Result r = a.ask("roll two dice");
        const int total = std::stoi(*r.slot("total"));
        CHECK(total >= 2);
        CHECK(total <= 12);
        CHECK(*r.slot("dice") == "2");
    }
    const Result d20 = a.ask("roll a d20");
    CHECK(*d20.slot("sides") == "20");
    CHECK(std::stoi(*d20.slot("total")) <= 20);
    CHECK(*a.ask("roll a twelve sided die").slot("sides") == "12");
    CHECK(*a.ask("roll the dice").slot("dice") == "1");
}

// contacts

TEST_CASE("call: a contact by name, a nickname, a sound-alike and a misspelling") {
    Fixture f;
    Result r = f.ask("call mom");
    CHECK(r.route == Route::OnDevice);
    CHECK(r.action == "call.start");
    CHECK(*r.slot("contact_id") == "1");
    CHECK(r.reply == "Calling Mom.");
    CHECK(*f.ask("call jon").slot("contact_id") == "3");
    CHECK(*f.ask("call dana please").slot("contact_id") == "6");
    CHECK(*f.ask("could you call john smith for me").slot("contact_id") == "2");
}

TEST_CASE("call: two equally good matches are asked about, an unknown name is reported, no name is asked for") {
    Fixture f;
    const Result both = f.ask("call sarah conner");
    CHECK(both.route == Route::Clarify);
    CHECK(both.reason == "ambiguous_contact");
    CHECK(both.reply.find("Sarah Connor") != std::string::npos);
    CHECK(both.reply.find("Sara Conner") != std::string::npos);
    REQUIRE(both.slot("candidate_1") != nullptr);

    const Result unknown = f.ask("call zed");
    CHECK(unknown.route == Route::Clarify);
    CHECK(unknown.reason == "contact_not_found");
    CHECK(unknown.reply.find("zed") != std::string::npos);

    const Result nobody = f.ask("call");
    CHECK(nobody.route == Route::Clarify);
    CHECK(nobody.reason == "missing_contact");
}

TEST_CASE("text: the contact and the message are pulled out of the sentence") {
    Fixture f;
    Result r = f.ask("text sarah connor saying i am running late");
    CHECK(r.route == Route::OnDevice);
    CHECK(r.action == "text.send");
    CHECK(*r.slot("contact_id") == "4");
    CHECK(*r.slot("body") == "i am running late");

    r = f.ask("text mom tell her i will be late");
    CHECK(*r.slot("contact_id") == "1");
    CHECK(*r.slot("body") == "i will be late");

    r = f.ask("text dana that dinner is at seven");
    CHECK(*r.slot("body") == "dinner is at seven");
}

TEST_CASE("text: no message is asked for, with the contact kept") {
    Fixture f;
    const Result r = f.ask("text mom");
    CHECK(r.route == Route::Clarify);
    CHECK(r.reason == "missing_message");
    CHECK(*r.slot("contact_id") == "1");
}

TEST_CASE("changing the contact list changes who can be called, and an engine forked earlier keeps the old list") {
    Fixture f;
    Engine before = f.engine.fork();
    f.engine.set_contacts({{9, "Zed Zimmer"}});
    CHECK(*f.engine.handle("call zed").slot("contact_id") == "9");
    CHECK(f.engine.handle("call mom").reason == "contact_not_found");
    CHECK(*before.handle("call mom").slot("contact_id") == "1");
    CHECK(f.engine.contact_count() == 1);
    CHECK(before.contact_count() == 6);
}

// reminders

TEST_CASE("reminder: the task and the time are separated") {
    Fixture f;
    Result r = f.ask("remind me to buy milk at 5 pm");
    CHECK(r.route == Route::OnDevice);
    CHECK(*r.slot("task") == "buy milk");
    CHECK(*r.slot("at") == "2026-10-08T17:00");
    CHECK(r.reply == "I'll remind you to buy milk today at 5:00 PM.");

    r = f.ask("remind me to phone dana tomorrow at 9");
    CHECK(*r.slot("task") == "phone dana");
    CHECK(*r.slot("at") == "2026-10-09T09:00");

    r = f.ask("remind me in ten minutes to stretch");
    CHECK(*r.slot("task") == "stretch");
    CHECK(*r.slot("at") == "2026-10-08T09:25");

    r = f.ask("remind me to water the plants on friday");
    CHECK(*r.slot("task") == "water the plants");
    CHECK(*r.slot("at") == "2026-10-09T00:00");
}

TEST_CASE("reminder: missing pieces are asked for") {
    Fixture f;
    Result r = f.ask("remind me to buy milk");
    CHECK(r.route == Route::Clarify);
    CHECK(r.reason == "missing_time");
    CHECK(*r.slot("task") == "buy milk");

    r = f.ask("remind me");
    CHECK(r.route == Route::Clarify);
    CHECK(r.reason == "missing_task");

    r = f.ask("remind me to buy milk at 7 am today");
    CHECK(r.reason == "time_passed");
}

// small talk

TEST_CASE("greetings are answered on the device") {
    Fixture f;
    CHECK(f.ask("hello there").reply == "Hello.");
    CHECK(f.ask("goodbye").reply == "Goodbye.");
    CHECK(f.ask("thanks a lot").reply == "You're welcome.");
}

// escalation

TEST_CASE("escalation: a request with no local handler goes on, with personal data removed") {
    Fixture f;
    const Result r = f.ask("weather for sarah 415-555-2671 email sarah@home.com");
    CHECK(r.route == Route::Escalate);
    CHECK(r.reason == "no_local_handler");
    CHECK(r.intent == "weather");
    CHECK(r.forward_text == "weather for <NAME> <PHONE> email <EMAIL>");
}

TEST_CASE("escalation: a request the model is not sure about goes on") {
    Fixture f;
    const Result r = f.ask("blah blah blah mom");
    CHECK(r.route == Route::Escalate);
    CHECK(r.reason == "low_confidence");
    CHECK(r.confidence < 0.6F);
    CHECK(r.forward_text == "blah blah blah <NAME>");
}

TEST_CASE("escalation: the threshold is a setting, and a threshold above one sends everything on") {
    EngineConfig strict;
    strict.handle_threshold = 1.1F;
    Fixture f(strict);
    const Result r = f.ask("set a timer for ten minutes");
    CHECK(r.route == Route::Escalate);
    CHECK(r.reason == "low_confidence");
    CHECK(r.intent == "timer");
}

TEST_CASE("escalation: nothing is forwarded for an empty utterance") {
    Fixture f;
    for (const char* text : {"", "   ", "?!"}) {
        const Result r = f.ask(text);
        CHECK(r.route == Route::Escalate);
        CHECK(r.reason == "empty_utterance");
        CHECK(r.forward_text.empty());
    }
}

TEST_CASE("escalation: only forward_text can carry the user's words off the device") {
    Fixture f;
    for (const char* text : {"weather for mom 415-555-2671", "blah dana", "set a timer for ten minutes", "call mom"}) {
        const Result r = f.ask(text);
        if (r.route == Route::Escalate) {
            CHECK(r.forward_text.find("415") == std::string::npos);
            CHECK(r.forward_text.find("mom") == std::string::npos);
            CHECK(r.forward_text.find("dana") == std::string::npos);
        } else {
            CHECK(r.forward_text.empty());
        }
    }
}

// the engine itself

TEST_CASE("engine: classify exposes the model's answer, and forks give the same answers") {
    Fixture f;
    const Prediction p = f.engine.classify("set a timer for ten minutes");
    CHECK(f.engine.model().class_name(p.intent) == "timer");
    Engine other = f.engine.fork();
    CHECK(other.handle("set a timer for ten minutes").reply == f.ask("set a timer for ten minutes").reply);
}

TEST_CASE("engine: a hostile utterance of every length and byte is handled without a crash") {
    Fixture f;
    std::uint32_t state = 17;
    auto next = [&] {
        state = state * 1664525U + 1013904223U;
        return state >> 8;
    };
    for (int i = 0; i < 3000; ++i) {
        std::string s;
        for (unsigned n = next() % 700; n > 0; --n) s.push_back(static_cast<char>(next() % 256));
        const Result r = f.ask(s);
        CHECK((r.route == Route::OnDevice || r.route == Route::Clarify || r.route == Route::Escalate));
    }
    for (const char* text : {"call timer wake time", "timer timer timer timer", "remind remind", "text", "text text text"}) {
        CHECK_NOTHROW(f.ask(text));
    }
}
