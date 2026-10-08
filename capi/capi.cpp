#include "assist.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "assist/engine.hpp"
#include "assist/json.hpp"
#include "assist/stream.hpp"

struct assist_engine {
    explicit assist_engine(assist::Engine e) : engine(std::move(e)) {}
    assist::Engine engine;
    std::mutex mutex;  // Engine keeps scratch buffers, so one call at a time
};

struct assist_stream {
    assist_stream(assist::Engine e, assist_partial_fn fn, void* user) : on_partial(fn), user_data(user) {
        session = std::make_unique<assist::StreamSession>(std::move(e), [this](const assist::Result& r, std::uint64_t sequence) { deliver(r, sequence); });
    }

    // Held while the host callback runs, so cancel() cannot return while a callback is still in progress.
    void deliver(const assist::Result& result, std::uint64_t sequence) {
        std::lock_guard lock(callback_mutex);
        if (cancelled || on_partial == nullptr) return;
        const std::string json = assist::to_json(result);
        on_partial(json.c_str(), json.size(), sequence, user_data);
    }

    void cancel() {
        {
            std::lock_guard lock(callback_mutex);
            cancelled = true;
        }
        session->cancel();
    }

    assist_partial_fn on_partial;
    void* user_data;
    std::mutex callback_mutex;
    bool cancelled = false;   // guarded by callback_mutex
    bool finished = false;    // guarded by control_mutex
    std::mutex control_mutex;
    std::unique_ptr<assist::StreamSession> session;
};

namespace {

assist_status give_string(const std::string& text, char** out, size_t* out_len) {
    char* buffer = static_cast<char*>(std::malloc(text.size() + 1));
    if (buffer == nullptr) return ASSIST_ERR_NO_MEMORY;
    std::memcpy(buffer, text.c_str(), text.size() + 1);
    *out = buffer;
    if (out_len != nullptr) *out_len = text.size();
    return ASSIST_OK;
}

assist_status map_load(assist::LoadStatus s) {
    switch (s) {
        case assist::LoadStatus::Ok: return ASSIST_OK;
        case assist::LoadStatus::IoError: return ASSIST_ERR_IO;
        case assist::LoadStatus::BadChecksum: return ASSIST_ERR_CHECKSUM;
        default: return ASSIST_ERR_FORMAT;
    }
}

template <class F>
assist_status guarded(F&& body) noexcept {
    try {
        return body();
    } catch (const std::bad_alloc&) {
        return ASSIST_ERR_NO_MEMORY;
    } catch (...) {
        return ASSIST_ERR_INTERNAL;
    }
}

}  // namespace

extern "C" {

const char* assist_version(void) { return "1.0"; }

const char* assist_status_message(assist_status status) {
    switch (status) {
        case ASSIST_OK: return "ok";
        case ASSIST_ERR_ARGUMENT: return "invalid argument";
        case ASSIST_ERR_IO: return "the model file could not be read";
        case ASSIST_ERR_FORMAT: return "the file is not a valid model";
        case ASSIST_ERR_CHECKSUM: return "the model file is damaged (checksum mismatch)";
        case ASSIST_ERR_STOPPED: return "the stream has already finished or was cancelled";
        case ASSIST_ERR_NO_MEMORY: return "out of memory";
        case ASSIST_ERR_INTERNAL: return "internal error";
    }
    return "unknown status";
}

assist_status assist_engine_create(const assist_options* options, assist_engine** out) {
    if (out != nullptr) *out = nullptr;
    if (options == nullptr || out == nullptr || options->model_path == nullptr || options->struct_size < sizeof(assist_options)) return ASSIST_ERR_ARGUMENT;
    return guarded([&]() -> assist_status {
        assist::Model model;
        if (const auto s = assist::Model::load(options->model_path, model, options->verify_checksum != 0); s != assist::LoadStatus::Ok) return map_load(s);

        std::shared_ptr<const assist::Clock> clock;
        if (options->use_fixed_time != 0) {
            if (options->year < 1 || options->year > 9999 || options->month < 1 || options->month > 12 || options->day < 1 || options->hour < 0 || options->hour > 23 || options->minute < 0 || options->minute > 59) return ASSIST_ERR_ARGUMENT;
            const auto month = static_cast<unsigned>(options->month);
            if (static_cast<unsigned>(options->day) > assist::days_in_month(options->year, month)) return ASSIST_ERR_ARGUMENT;
            clock = std::make_shared<assist::FixedClock>(assist::LocalTime{options->year, month, static_cast<unsigned>(options->day),
                                                                           static_cast<unsigned>(options->hour), static_cast<unsigned>(options->minute), 0});
        } else {
            clock = std::make_shared<assist::SystemClock>();
        }
        assist::EngineConfig config;
        config.random_seed = options->random_seed;
        *out = new assist_engine(assist::Engine(std::move(model), std::move(clock), config));
        return ASSIST_OK;
    });
}

assist_status assist_engine_fork(const assist_engine* engine, assist_engine** out) {
    if (out != nullptr) *out = nullptr;
    if (engine == nullptr || out == nullptr) return ASSIST_ERR_ARGUMENT;
    return guarded([&]() -> assist_status {
        auto* source = const_cast<assist_engine*>(engine);
        std::lock_guard lock(source->mutex);
        *out = new assist_engine(source->engine.fork());
        return ASSIST_OK;
    });
}

void assist_engine_destroy(assist_engine* engine) { delete engine; }

assist_status assist_engine_set_contacts(assist_engine* engine, const assist_contact* contacts, size_t count, size_t* rejected) {
    if (engine == nullptr || (count > 0 && contacts == nullptr)) return ASSIST_ERR_ARGUMENT;
    return guarded([&]() -> assist_status {
        std::vector<std::pair<std::uint32_t, std::string>> list;
        list.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (contacts[i].name == nullptr) return ASSIST_ERR_ARGUMENT;
            list.emplace_back(contacts[i].id, contacts[i].name);
        }
        std::lock_guard lock(engine->mutex);
        engine->engine.set_contacts(list);
        if (rejected != nullptr) *rejected = count - engine->engine.contact_count();
        return ASSIST_OK;
    });
}

assist_status assist_engine_handle(assist_engine* engine, const char* utf8, size_t utf8_len, char** json, size_t* json_len) {
    if (json != nullptr) *json = nullptr;
    if (engine == nullptr || json == nullptr || (utf8 == nullptr && utf8_len > 0)) return ASSIST_ERR_ARGUMENT;
    return guarded([&]() -> assist_status {
        std::string out;
        {
            std::lock_guard lock(engine->mutex);
            out = assist::to_json(engine->engine.handle(std::string_view(utf8 == nullptr ? "" : utf8, utf8_len)));
        }
        return give_string(out, json, json_len);
    });
}

void assist_string_free(char* json) { std::free(json); }

assist_status assist_stream_create(const assist_engine* engine, assist_partial_fn on_partial, void* user_data, assist_stream** out) {
    if (out != nullptr) *out = nullptr;
    if (engine == nullptr || out == nullptr) return ASSIST_ERR_ARGUMENT;
    return guarded([&]() -> assist_status {
        auto* source = const_cast<assist_engine*>(engine);
        assist::Engine copy = [&] {
            std::lock_guard lock(source->mutex);
            return source->engine.fork();
        }();
        *out = new assist_stream(std::move(copy), on_partial, user_data);
        return ASSIST_OK;
    });
}

assist_status assist_stream_update(assist_stream* stream, const char* utf8, size_t utf8_len) {
    if (stream == nullptr || (utf8 == nullptr && utf8_len > 0)) return ASSIST_ERR_ARGUMENT;
    return guarded([&]() -> assist_status {
        {
            std::lock_guard lock(stream->control_mutex);
            if (stream->finished) return ASSIST_ERR_STOPPED;
        }
        stream->session->update(std::string_view(utf8 == nullptr ? "" : utf8, utf8_len));
        return ASSIST_OK;
    });
}

assist_status assist_stream_finish(assist_stream* stream, char** json, size_t* json_len) {
    if (json != nullptr) *json = nullptr;
    if (stream == nullptr || json == nullptr) return ASSIST_ERR_ARGUMENT;
    return guarded([&]() -> assist_status {
        {
            std::lock_guard lock(stream->control_mutex);
            if (stream->finished) return ASSIST_ERR_STOPPED;
            stream->finished = true;
        }
        {
            std::lock_guard lock(stream->callback_mutex);
            if (stream->cancelled) return ASSIST_ERR_STOPPED;
        }
        return give_string(assist::to_json(stream->session->finish()), json, json_len);
    });
}

void assist_stream_cancel(assist_stream* stream) {
    if (stream == nullptr) return;
    try {
        stream->cancel();
    } catch (...) {
    }
}

void assist_stream_destroy(assist_stream* stream) {
    if (stream == nullptr) return;
    try {
        stream->cancel();
    } catch (...) {
    }
    delete stream;
}

}  // extern "C"
