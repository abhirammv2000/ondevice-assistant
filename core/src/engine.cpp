#include "assist/engine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

#include "assist/calc.hpp"
#include "assist/slots.hpp"

namespace assist {

const char* to_string(Route route) noexcept {
    switch (route) {
        case Route::OnDevice: return "on_device";
        case Route::Clarify: return "clarify";
        case Route::Escalate: return "escalate";
    }
    return "unknown";
}

// Everything a handler needs, so each handler is one small function.
struct HandlerContext {
    Engine& engine;
    Result result;         // starts with the intent, confidence and margin filled in
    Tokens tokens;
    LocalTime now;
    std::string_view utterance;

    const EngineConfig& config() const { return engine.shared_->config; }
    std::vector<FuzzyMatch> contacts(const std::string& words) { return engine.find_contacts(words); }
    std::string_view contact_name(std::uint32_t id) const { return engine.shared_->contacts.name_for_id(id); }
    std::mt19937_64& rng() { return engine.rng_; }
};

namespace {

constexpr std::array<const char*, 7> kWeekdays{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
constexpr std::array<const char*, 12> kMonths{"January", "February", "March", "April", "May", "June", "July",
                                              "August", "September", "October", "November", "December"};

std::string two(unsigned n) { return (n < 10 ? "0" : "") + std::to_string(n); }

std::string clock_12h(unsigned hour, unsigned minute) {
    const unsigned h12 = hour % 12 == 0 ? 12 : hour % 12;
    return std::to_string(h12) + ":" + two(minute) + (hour < 12 ? " AM" : " PM");
}

std::string date_long(std::int64_t days) {
    std::int64_t y;
    unsigned m, d;
    civil_from_days(days, y, m, d);
    return std::string(kWeekdays[weekday_from_days(days)]) + ", " + kMonths[m - 1] + " " + std::to_string(d) + ", " + std::to_string(y);
}

std::string date_iso(std::int64_t days) {
    std::int64_t y;
    unsigned m, d;
    civil_from_days(days, y, m, d);
    return std::to_string(y) + "-" + two(m) + "-" + two(d);
}

std::string plural(std::int64_t n, const char* unit) { return std::to_string(n) + " " + unit + (n == 1 ? "" : "s"); }

// 5400 becomes "1 hour 30 minutes"
std::string humanize(std::int64_t seconds) {
    std::string out;
    auto add = [&](std::int64_t n, const char* unit) {
        if (n == 0) return;
        if (!out.empty()) out += " ";
        out += plural(n, unit);
    };
    add(seconds / 86400, "day");
    add(seconds % 86400 / 3600, "hour");
    add(seconds % 3600 / 60, "minute");
    add(seconds % 60, "second");
    return out.empty() ? "0 seconds" : out;
}

Result clarify(HandlerContext& c, std::string action, std::string reply, std::string reason) {
    Result r = std::move(c.result);
    r.route = Route::Clarify;
    r.action = std::move(action);
    r.reply = std::move(reply);
    r.reason = std::move(reason);
    return r;
}

Result done(HandlerContext& c, std::string action, std::string reply, std::vector<Slot> slots = {}) {
    Result r = std::move(c.result);
    r.route = Route::OnDevice;
    r.action = std::move(action);
    r.reply = std::move(reply);
    r.slots = std::move(slots);
    r.reason = "handled";
    return r;
}

Result escalate(HandlerContext& c, std::string reason) {
    Result r = std::move(c.result);
    r.route = Route::Escalate;
    r.reason = std::move(reason);
    return r;
}

bool is_word(Tokens t, std::size_t i, std::string_view w) { return i < t.size() && t[i].kind == TokKind::Word && t[i].text == w; }

bool has_word(Tokens t, std::string_view w) {
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (is_word(t, i, w)) return true;
    }
    return false;
}

// when something should happen

struct When {
    LocalTime at;
    std::size_t begin;  // where the phrase starts in the tokens, so a task can be cut off before it
    bool passed;        // the requested moment is already behind us
};

// "in ten minutes", "at 7 tomorrow", "friday at noon", "on may 3rd at 6 pm". Returns nothing if no time is named.
std::optional<When> resolve_when(Tokens t, const LocalTime& now) {
    const bool tonight = has_word(t, "tonight");
    if (const auto d = find_duration(t)) {
        const bool dated = find_date(t, now).has_value();  // "in 3 days" is a date, not a length of time
        if (!dated || d->seconds < 86400) {
            std::size_t begin = d->begin;
            if (begin > 0 && is_word(t, begin - 1, "in")) --begin;
            return When{LocalTime::from_seconds(now.seconds() + d->seconds), begin, false};
        }
    }
    auto tod = find_time_of_day(t);
    const auto date = find_date(t, now);
    if (!tod && !date) return std::nullopt;
    if (!tod) {
        // a date alone means that day, at the start of it
        return When{LocalTime::from_seconds(date->day * 86400), date->begin, date->day < now.days()};
    }
    if (tonight && tod->ambiguous) {
        tod->hour = tod->hour % 12 + 12;
        tod->ambiguous = false;
    }
    std::size_t begin = tod->begin;
    if (!date) return When{next_occurrence(now, *tod), begin, false};
    begin = std::min(begin, date->begin);

    unsigned hours[2];
    int n = 0;
    if (tod->ambiguous) {
        hours[n++] = tod->hour % 12;
        hours[n++] = tod->hour % 12 + 12;
    } else {
        hours[n++] = tod->hour;
    }
    std::int64_t best = -1;
    for (int i = 0; i < n; ++i) {
        const std::int64_t candidate = date->day * 86400 + hours[i] * 3600 + tod->minute * 60;
        if (candidate > now.seconds()) {
            best = candidate;
            break;  // hours are in increasing order, so the first one still ahead is the earliest
        }
    }
    if (best < 0) return When{LocalTime::from_seconds(date->day * 86400 + hours[n - 1] * 3600 + tod->minute * 60), begin, true};
    return When{LocalTime::from_seconds(best), begin, false};
}

std::string describe(const LocalTime& at, const LocalTime& now) {
    const std::int64_t days = at.days();
    std::string day;
    if (days == now.days()) day = "today";
    else if (days == now.days() + 1) day = "tomorrow";
    else day = date_long(days);
    return day + " at " + clock_12h(at.hour, at.minute);
}

std::string iso_time(const LocalTime& at) { return date_iso(at.days()) + "T" + two(at.hour) + ":" + two(at.minute); }

// words, for the handlers that pull a name or a task out of the sentence

std::vector<std::string_view> words_of(Tokens t) {
    std::vector<std::string_view> w;
    for (const SlotToken& k : t) {
        if (k.kind == TokKind::Word) w.push_back(k.text);
    }
    return w;
}

bool contains(std::initializer_list<std::string_view> set, std::string_view w) { return std::find(set.begin(), set.end(), w) != set.end(); }

std::string join(const std::vector<std::string_view>& w, std::size_t from, std::size_t to) {
    std::string out;
    for (std::size_t i = from; i < to && i < w.size(); ++i) {
        if (!out.empty()) out += ' ';
        out += w[i];
    }
    return out;
}

// handlers

Result handle_timer(HandlerContext& c) {
    const auto d = find_duration(c.tokens);
    if (!d) return clarify(c, "timer.set", "For how long?", "missing_duration");
    return done(c, "timer.set", "Timer set for " + humanize(d->seconds) + ".", {{"seconds", std::to_string(d->seconds)}});
}

Result handle_alarm(HandlerContext& c) {
    const auto when = resolve_when(c.tokens, c.now);
    if (!when) return clarify(c, "alarm.set", "What time should I set the alarm for?", "missing_time");
    if (when->passed) return clarify(c, "alarm.set", "That time has already passed. When should I set it for?", "time_passed");
    return done(c, "alarm.set", "Alarm set for " + describe(when->at, c.now) + ".", {{"at", iso_time(when->at)}});
}

Result handle_time(HandlerContext& c) {
    if (has_word(c.tokens, "in")) return escalate(c, "needs_timezone_data");  // "what time is it in Tokyo"
    return done(c, "", "It is " + clock_12h(c.now.hour, c.now.minute) + ".");
}

Result handle_date(HandlerContext& c) {
    const auto d = find_date(c.tokens, c.now);
    const std::int64_t day = d ? d->day : c.now.days();
    std::string lead = "That is ";
    if (day == c.now.days()) lead = "Today is ";
    else if (day == c.now.days() + 1) lead = "Tomorrow is ";
    else if (day == c.now.days() - 1) lead = "Yesterday was ";
    return done(c, "", lead + date_long(day) + ".", {{"date", date_iso(day)}});
}

Result handle_calculator(HandlerContext& c) {
    const CalcResult r = evaluate_spoken(c.tokens);
    switch (r.status) {
        case CalcStatus::Ok: return done(c, "", format_number(r.value) + ".", {{"value", format_number(r.value)}});
        case CalcStatus::DivideByZero: return done(c, "", "I can't divide by zero.");
        case CalcStatus::NoExpression:
        case CalcStatus::NotFinite: return escalate(c, "no_arithmetic_found");
    }
    return escalate(c, "no_arithmetic_found");
}

Result handle_flip_coin(HandlerContext& c) {
    const bool heads = (c.rng()() & 1U) == 0;
    return done(c, "", heads ? "Heads." : "Tails.", {{"result", heads ? "heads" : "tails"}});
}

Result handle_roll_dice(HandlerContext& c) {
    int count = 1;
    int sides = 6;
    const Tokens t = c.tokens;
    for (std::size_t i = 0; i < t.size(); ++i) {
        // "two dice", "3 dice"
        if ((is_word(t, i, "dice") || is_word(t, i, "die")) && i > 0) {
            if (const auto n = parse_number(t, i - 1); n && n->end == i && n->value >= 1 && n->value <= 10) count = static_cast<int>(n->value);
        }
        // "d20", "a d 12"
        if (is_word(t, i, "d") && i + 1 < t.size() && t[i + 1].kind == TokKind::Number && t[i + 1].value >= 2 && t[i + 1].value <= 1000) {
            sides = static_cast<int>(t[i + 1].value);
        }
        // "20 sided", "twenty sided"
        if (is_word(t, i, "sided") && i > 0) {
            if (const auto n = parse_number(t, i - 1); n && n->end == i && n->value >= 2 && n->value <= 1000) sides = static_cast<int>(n->value);
        }
    }
    std::uniform_int_distribution<int> die(1, sides);
    std::string rolls;
    int total = 0;
    for (int i = 0; i < count; ++i) {
        const int v = die(c.rng());
        total += v;
        if (i > 0) rolls += i + 1 == count ? " and " : ", ";
        rolls += std::to_string(v);
    }
    std::string reply = "You rolled " + rolls;
    if (count > 1) reply += ", for a total of " + std::to_string(total);
    return done(c, "", reply + ".", {{"total", std::to_string(total)}, {"dice", std::to_string(count)}, {"sides", std::to_string(sides)}});
}

// the contact in a sentence, resolved, or the Result that asks about it
struct ContactChoice {
    std::optional<std::uint32_t> id;
    std::optional<Result> ask;
};

ContactChoice choose_contact(HandlerContext& c, const std::string& name, const char* action, const char* question) {
    if (name.empty()) return {std::nullopt, clarify(c, action, question, "missing_contact")};
    auto matches = c.contacts(name);
    matches.erase(std::remove_if(matches.begin(), matches.end(), [&](const FuzzyMatch& m) { return m.score < c.config().contact_threshold; }), matches.end());
    if (matches.empty()) {
        return {std::nullopt, clarify(c, action, "I couldn't find \"" + name + "\" in your contacts. " + question, "contact_not_found")};
    }
    if (matches.size() > 1 && matches[0].score - matches[1].score < c.config().ambiguity_gap) {
        Result r = clarify(c, action,
                           "Which one: " + std::string(c.contact_name(matches[0].id)) + " or " + std::string(c.contact_name(matches[1].id)) + "?",
                           "ambiguous_contact");
        r.slots = {{"candidate_1", std::to_string(matches[0].id)}, {"candidate_2", std::to_string(matches[1].id)}};
        return {std::nullopt, std::move(r)};
    }
    return {matches[0].id, std::nullopt};
}

Result handle_call(HandlerContext& c) {
    const auto w = words_of(c.tokens);
    std::vector<std::string_view> name;
    for (const auto word : w) {
        if (!contains({"call", "phone", "ring", "dial", "make", "a", "an", "to", "talk", "speak", "with", "i", "want", "must", "need", "please", "can",
                       "you", "could", "would", "give", "me", "the", "my", "now", "up", "hey", "and", "get", "connect", "contact", "for"}, word)) {
            name.push_back(word);
        }
    }
    const std::string query = join(name, 0, 4);
    auto choice = choose_contact(c, query, "call.start", "Who do you want to call?");
    if (choice.ask) return std::move(*choice.ask);
    const std::string display(c.contact_name(*choice.id));
    return done(c, "call.start", "Calling " + display + ".", {{"contact_id", std::to_string(*choice.id)}, {"name", display}});
}

Result handle_text(HandlerContext& c) {
    const auto w = words_of(c.tokens);
    // the verb, then the name up to a word that starts the message, then the message
    std::size_t verb = w.size();
    for (std::size_t i = 0; i < w.size(); ++i) {
        if (contains({"text", "message", "sms", "texting"}, w[i])) {
            verb = i;
            break;
        }
    }
    std::size_t start = verb == w.size() ? 0 : verb + 1;
    std::size_t marker = w.size();
    for (std::size_t i = start; i < w.size(); ++i) {
        if (contains({"saying", "say", "that", "tell", "telling", "and", "about"}, w[i])) {
            marker = i;
            break;
        }
    }
    std::vector<std::string_view> name;
    for (std::size_t i = start; i < marker; ++i) {
        if (!contains({"a", "an", "to", "i", "want", "please", "can", "you", "could", "would", "my", "the", "hey", "for", "me", "send"}, w[i])) name.push_back(w[i]);
    }
    std::size_t body_from = marker < w.size() ? marker + 1 : w.size();
    if (marker < w.size() && contains({"tell", "telling"}, w[marker]) && body_from < w.size() && contains({"them", "him", "her", "that"}, w[body_from])) ++body_from;
    if (body_from < w.size() && w[body_from] == "that") ++body_from;
    const std::string body = join(w, body_from, w.size());

    auto choice = choose_contact(c, join(name, 0, 4), "text.send", "Who do you want to text?");
    if (choice.ask) return std::move(*choice.ask);
    const std::string display(c.contact_name(*choice.id));
    if (body.empty()) {
        Result r = clarify(c, "text.send", "What do you want to say to " + display + "?", "missing_message");
        r.slots = {{"contact_id", std::to_string(*choice.id)}, {"name", display}};
        return r;
    }
    return done(c, "text.send", "Sending to " + display + ": " + body, {{"contact_id", std::to_string(*choice.id)}, {"name", display}, {"body", body}});
}

Result handle_reminder(HandlerContext& c) {
    const auto w = words_of(c.tokens);
    const auto when = resolve_when(c.tokens, c.now);
    // the task starts after "to" or "about" and runs up to where the time begins
    std::size_t start = w.size();
    for (std::size_t i = 0; i < w.size(); ++i) {
        if (contains({"remind", "reminder"}, w[i])) {
            for (std::size_t j = i + 1; j < w.size(); ++j) {
                if (contains({"to", "about"}, w[j])) {
                    start = j + 1;
                    break;
                }
            }
            break;
        }
    }
    // token positions and word positions differ only by numbers and clock times, so cut by matching text instead
    std::string task;
    if (start < w.size()) {
        std::size_t end = w.size();
        if (when) {
            // the word index where the time phrase begins. A time that comes before the task ("remind me in ten
            // minutes to stretch") does not cut it off. A time after it does ("remind me to stretch at five").
            std::size_t words_seen = 0;
            for (std::size_t i = 0; i < c.tokens.size() && i < when->begin; ++i) {
                if (c.tokens[i].kind == TokKind::Word) ++words_seen;
            }
            if (words_seen >= start) end = std::min<std::size_t>(end, words_seen);
        }
        while (end > start && contains({"at", "on", "in", "by", "for", "this", "next", "tomorrow", "today", "tonight"}, w[end - 1])) --end;
        task = join(w, start, end);
    }
    if (task.empty()) return clarify(c, "reminder.create", "What should I remind you about?", "missing_task");
    if (!when) {
        Result r = clarify(c, "reminder.create", "When should I remind you?", "missing_time");
        r.slots = {{"task", task}};
        return r;
    }
    if (when->passed) {
        Result r = clarify(c, "reminder.create", "That time has already passed. When should I remind you?", "time_passed");
        r.slots = {{"task", task}};
        return r;
    }
    return done(c, "reminder.create", "I'll remind you to " + task + " " + describe(when->at, c.now) + ".", {{"task", task}, {"at", iso_time(when->at)}});
}

Result handle_greeting(HandlerContext& c) { return done(c, "", "Hello."); }
Result handle_goodbye(HandlerContext& c) { return done(c, "", "Goodbye."); }
Result handle_thanks(HandlerContext& c) { return done(c, "", "You're welcome."); }

struct Handler {
    std::string_view intent;
    Result (*run)(HandlerContext&);
};

constexpr std::array<Handler, 13> kHandlers{{{"timer", handle_timer},
                                              {"alarm", handle_alarm},
                                              {"time", handle_time},
                                              {"date", handle_date},
                                              {"calculator", handle_calculator},
                                              {"flip_coin", handle_flip_coin},
                                              {"roll_dice", handle_roll_dice},
                                              {"make_call", handle_call},
                                              {"text", handle_text},
                                              {"reminder_update", handle_reminder},
                                              {"greeting", handle_greeting},
                                              {"goodbye", handle_goodbye},
                                              {"thank_you", handle_thanks}}};

}  // namespace

Engine::Engine(Model model, std::shared_ptr<const Clock> clock, EngineConfig config)
    : Engine([&] {
          auto s = std::make_shared<Shared>();
          s->model = std::move(model);
          s->clock = std::move(clock);
          s->config = config;
          s->contacts.finalize();
          return std::shared_ptr<const Shared>(std::move(s));
      }()) {}

Engine::Engine(std::shared_ptr<const Shared> shared)
    : shared_(std::move(shared)),
      scratch_(shared_->model.make_scratch()),
      contact_cache_(shared_->config.contact_cache_capacity),
      rng_(shared_->config.random_seed != 0 ? shared_->config.random_seed : std::random_device{}()) {}

Engine Engine::fork() const {
    Engine e(shared_);
    e.rng_.seed(rng_());  // a different but reproducible stream when the seed was fixed
    return e;
}

void Engine::set_contacts(const std::vector<std::pair<std::uint32_t, std::string>>& contacts) {
    auto next = std::make_shared<Shared>(*shared_);
    next->contacts = FuzzyIndex{};
    next->redactor = Redactor{};
    for (const auto& [id, name] : contacts) {
        if (next->contacts.add(id, name)) next->redactor.add_name(name);
    }
    next->contacts.finalize();
    shared_ = std::move(next);  // other engines forked earlier keep the list they had
    contact_cache_.clear();
}

std::vector<FuzzyMatch> Engine::find_contacts(const std::string& words) {
    if (const auto* hit = contact_cache_.get(words)) return *hit;
    auto found = shared_->contacts.query(words, 3, 0.5F);
    contact_cache_.put(words, found);
    return found;
}

Prediction Engine::classify(std::string_view utterance) {
    extract_features(utterance, tokens_);
    return shared_->model.predict(tokens_.feature_span(), scratch_);
}

Result Engine::handle(std::string_view utterance) {
    extract_features(utterance, tokens_);
    Result base;
    if (tokens_.feature_count == 0) {
        base.route = Route::Escalate;
        base.reason = "empty_utterance";
        return base;
    }
    const Prediction p = shared_->model.predict(tokens_.feature_span(), scratch_);
    base.intent = std::string(shared_->model.class_name(p.intent));
    base.confidence = p.confidence;
    base.margin = p.margin;

    auto finish_escalation = [&](Result r) {
        if (r.route == Route::Escalate) r.forward_text = shared_->redactor.redact(utterance).text;
        return r;
    };

    if (p.confidence < shared_->config.handle_threshold) {
        base.route = Route::Escalate;
        base.reason = "low_confidence";
        return finish_escalation(std::move(base));
    }
    const Handler* handler = nullptr;
    for (const Handler& h : kHandlers) {
        if (h.intent == base.intent) {
            handler = &h;
            break;
        }
    }
    if (!handler) {
        base.route = Route::Escalate;
        base.reason = "no_local_handler";
        return finish_escalation(std::move(base));
    }

    SlotText text;
    text.lex(utterance);
    HandlerContext ctx{*this, std::move(base), text.tokens(), shared_->clock->now(), utterance};
    return finish_escalation(handler->run(ctx));
}

}  // namespace assist
