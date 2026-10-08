// A command-line front end for trying the engine and for evaluating it in bulk.
//
//   assist_cli --model models/clinc150.pmodel "set a timer for ten minutes"      one request, the result as JSON
//   assist_cli --model m.pmodel --batch utterances.txt                           one request per line, a TSV of routes
//   assist_cli --model m.pmodel --classify utterances.txt                        only the model, a TSV of intents
//   assist_cli --model m.pmodel --contacts contacts.tsv --now 2026-10-08T09:15 "call mom"
//
// --contacts is a file of "id<TAB>name" lines. --now fixes the clock so a run can be repeated exactly.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "assist/engine.hpp"
#include "assist/json.hpp"

namespace {

[[noreturn]] void usage(const char* why) {
    std::fprintf(stderr, "%s\nusage: assist_cli --model FILE [--contacts FILE] [--now YYYY-MM-DDTHH:MM] [--seed N] [--no-rescue] (--batch FILE | --classify FILE | \"utterance\")\n", why);
    std::exit(2);
}

bool parse_now(const std::string& s, assist::LocalTime& out) {
    unsigned y, mo, d, h, mi;
    if (std::sscanf(s.c_str(), "%u-%u-%uT%u:%u", &y, &mo, &d, &h, &mi) != 5) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > assist::days_in_month(y, mo) || h > 23 || mi > 59) return false;
    out = assist::LocalTime{static_cast<std::int64_t>(y), mo, d, h, mi, 0};
    return true;
}

std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream in(path);
    if (!in) usage(("cannot read " + path).c_str());
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

}  // namespace

int main(int argc, char** argv) {
    std::string model_path, contacts_path, batch_path, classify_path, utterance;
    std::string now_text;
    std::uint64_t seed = 0;
    bool have_utterance = false;
    bool rescue = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) usage(("missing value for " + a).c_str());
            return argv[++i];
        };
        if (a == "--model") model_path = next();
        else if (a == "--contacts") contacts_path = next();
        else if (a == "--batch") batch_path = next();
        else if (a == "--classify") classify_path = next();
        else if (a == "--now") now_text = next();
        else if (a == "--seed") seed = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--no-rescue") rescue = false;
        else if (!a.empty() && a[0] == '-') usage(("unknown option " + a).c_str());
        else {
            utterance = a;
            have_utterance = true;
        }
    }
    if (model_path.empty()) usage("--model is required");

    assist::Model model;
    if (const auto status = assist::Model::load(model_path, model); status != assist::LoadStatus::Ok) {
        std::fprintf(stderr, "cannot load %s: %s\n", model_path.c_str(), assist::to_string(status));
        return 1;
    }

    std::shared_ptr<assist::Clock> clock = std::make_shared<assist::SystemClock>();
    if (!now_text.empty()) {
        assist::LocalTime t;
        if (!parse_now(now_text, t)) usage("--now must look like 2026-10-08T09:15");
        clock = std::make_shared<assist::FixedClock>(t);
    }
    assist::EngineConfig config;
    config.random_seed = seed;
    config.arithmetic_rescue = rescue;
    assist::Engine engine(std::move(model), clock, config);

    if (!contacts_path.empty()) {
        std::vector<std::pair<std::uint32_t, std::string>> contacts;
        for (const auto& line : read_lines(contacts_path)) {
            const auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            contacts.emplace_back(static_cast<std::uint32_t>(std::strtoul(line.substr(0, tab).c_str(), nullptr, 10)), line.substr(tab + 1));
        }
        engine.set_contacts(contacts);
    }

    if (!classify_path.empty()) {
        for (const auto& line : read_lines(classify_path)) {
            const assist::Prediction p = engine.classify(line);
            std::printf("%.*s\t%.6f\n", static_cast<int>(engine.model().class_name(p.intent).size()), engine.model().class_name(p.intent).data(),
                        static_cast<double>(p.confidence));
        }
        return 0;
    }
    if (!batch_path.empty()) {
        for (const auto& line : read_lines(batch_path)) {
            const assist::Result r = engine.handle(line);
            std::printf("%s\t%.6f\t%s\t%s\t%s\n", r.intent.c_str(), static_cast<double>(r.confidence), assist::to_string(r.route), r.reason.c_str(), r.action.c_str());
        }
        return 0;
    }
    if (!have_utterance) usage("give an utterance, --batch or --classify");
    std::printf("%s\n", assist::to_json(engine.handle(utterance)).c_str());
    return 0;
}
