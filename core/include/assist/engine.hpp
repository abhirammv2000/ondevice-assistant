// The request path: classify an utterance, decide whether the device can handle it, and either do the work or
// prepare the request for a larger model elsewhere.
//
//   utterance --> features --> intent + confidence --> handler on the device --> Result (OnDevice or Clarify)
//                                   |                        |
//                                   +-- low confidence ------+-- no handler, or it cannot complete --> Result (Escalate)
//
// OnDevice means the device did the work and the Result holds the action for the host to carry out and a reply to
// show. Clarify means the device understood the request but a value is missing, and the reply asks for it. Escalate
// means it should go elsewhere, and the Result carries the utterance with personal data removed. The device never
// sends anything itself. The host decides whether and where an escalated request goes, which keeps this library free
// of any network code and makes the privacy rule easy to see: the only text that can leave is Result::forward_text.
//
// An Engine is not thread-safe. Make one per thread with fork(), which shares the model, the contacts and the
// settings, which are read-only, and gives each thread its own scratch space and cache. The model file is mapped once.
#pragma once

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "assist/calendar.hpp"
#include "assist/fuzzy.hpp"
#include "assist/lru.hpp"
#include "assist/model.hpp"
#include "assist/redact.hpp"
#include "assist/tokenizer.hpp"

namespace assist {

enum class Route { OnDevice, Clarify, Escalate };

const char* to_string(Route route) noexcept;

struct Slot {
    std::string name;
    std::string value;
};

struct Result {
    Route route = Route::Escalate;
    std::string intent;        // the model's best guess
    float confidence = 0.0F;
    float margin = 0.0F;       // best probability minus the second best
    std::string action;        // what the host should do, such as "timer.set". Empty when there is nothing to do.
    std::vector<Slot> slots;   // the values for the action
    std::string reply;         // what to say or show
    std::string forward_text;  // for Escalate: the utterance with personal data removed
    std::string reason;        // why this route, for logs and tests

    const std::string* slot(std::string_view name) const noexcept {
        for (const Slot& s : slots) {
            if (s.name == name) return &s.value;
        }
        return nullptr;
    }
};

struct EngineConfig {
    // The model is trusted for a request only above this probability. 0.6 answers about 88% of in-scope requests at
    // about 95% accuracy, and catches about 76% of out-of-scope ones. docs/DESIGN.md has the curve.
    float handle_threshold = 0.6F;
    // A contact needs this score to be accepted, and a runner-up within `ambiguity_gap` of the best at a similar
    // score makes the device ask which one was meant.
    float contact_threshold = 0.7F;
    float ambiguity_gap = 0.05F;
    // When the model is unsure but the words contain a real calculation ("what is twelve times seven"), do the
    // sum anyway. The grammar is exact where the model is not, and a wrong sum is rare because it needs two
    // numbers and an operator. Turn it off to see the model on its own.
    bool arithmetic_rescue = true;
    std::size_t contact_cache_capacity = 128;
    std::uint64_t random_seed = 0;  // 0 means seed from the operating system
};

class Engine {
public:
    Engine(Model model, std::shared_ptr<const Clock> clock, EngineConfig config = {});

    // Replace the contact list. Names are matched fuzzily and removed from anything that leaves the device.
    void set_contacts(const std::vector<std::pair<std::uint32_t, std::string>>& contacts);

    Result handle(std::string_view utterance);

    // Another Engine for another thread, sharing everything read-only.
    Engine fork() const;

    const Model& model() const noexcept { return shared_->model; }
    std::size_t contact_count() const noexcept { return shared_->contacts.size(); }
    // Classification only, without handling. For tests and tools.
    Prediction classify(std::string_view utterance);

private:
    struct Shared {
        Model model;
        std::shared_ptr<const Clock> clock;
        EngineConfig config;
        FuzzyIndex contacts;
        Redactor redactor;
    };

    explicit Engine(std::shared_ptr<const Shared> shared);

    std::vector<FuzzyMatch> find_contacts(const std::string& words);

    std::shared_ptr<const Shared> shared_;
    TokenScratch tokens_;
    Model::Scratch scratch_;
    LruCache<std::string, std::vector<FuzzyMatch>> contact_cache_;
    mutable std::mt19937_64 rng_;  // mutable so fork() can draw the seed for the new engine

    friend struct HandlerContext;
};

}  // namespace assist
