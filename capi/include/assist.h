/* A plain C interface to the assistant core, so Swift, Objective-C, Java (JNI) and anything else with a C FFI can use it.
 *
 * The rules, so nothing has to be guessed:
 *   - Every function returns an assist_status. No C++ exception ever crosses this boundary.
 *   - Handles are opaque. Create with *_create, release with *_destroy. Destroying NULL is allowed.
 *   - Text going in is UTF-8 with an explicit byte length (it may contain NUL, and need not end with one).
 *   - Text coming out is a JSON document in a buffer the library allocated. Release it with assist_string_free.
 *   - An engine can be called from any thread, one call at a time (calls are serialized inside). For parallel work,
 *     create more engines with assist_engine_fork, which shares the model and the contact list.
 *   - A stream callback runs on the stream's own thread. It must not call assist_stream_destroy on its own stream.
 */
#ifndef ASSIST_H
#define ASSIST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(ASSIST_SHARED)
#define ASSIST_API __declspec(dllexport)
#else
#define ASSIST_API
#endif

#define ASSIST_VERSION_MAJOR 1
#define ASSIST_VERSION_MINOR 0

typedef enum assist_status {
    ASSIST_OK = 0,
    ASSIST_ERR_ARGUMENT = 1,  /* a NULL pointer, a bad size or an out-of-range value */
    ASSIST_ERR_IO = 2,        /* the model file could not be opened or read */
    ASSIST_ERR_FORMAT = 3,    /* the file is not a valid model: wrong magic, version, header or layout */
    ASSIST_ERR_CHECKSUM = 4,  /* the model file is damaged */
    ASSIST_ERR_STOPPED = 5,   /* the stream was cancelled or already finished */
    ASSIST_ERR_NO_MEMORY = 6,
    ASSIST_ERR_INTERNAL = 7
} assist_status;

typedef struct assist_engine assist_engine;
typedef struct assist_stream assist_stream;

typedef struct assist_options {
    uint32_t struct_size;   /* set to sizeof(assist_options), so the struct can grow later without breaking callers */
    const char* model_path; /* UTF-8 path to a .pmodel file */
    uint64_t random_seed;   /* 0 seeds from the operating system, anything else makes dice and coins repeatable */
    int32_t verify_checksum;/* nonzero reads the whole file once at load to check the CRC-32 */
    int32_t use_fixed_time; /* nonzero uses the fields below instead of the system clock (for tests and demos) */
    int32_t year;
    int32_t month;          /* 1 to 12 */
    int32_t day;            /* 1 to 31 */
    int32_t hour;           /* 0 to 23 */
    int32_t minute;         /* 0 to 59 */
} assist_options;

typedef struct assist_contact {
    uint32_t id;
    const char* name;       /* UTF-8, NUL-terminated */
} assist_contact;

ASSIST_API const char* assist_version(void);
ASSIST_API const char* assist_status_message(assist_status status);

ASSIST_API assist_status assist_engine_create(const assist_options* options, assist_engine** out);
ASSIST_API assist_status assist_engine_fork(const assist_engine* engine, assist_engine** out);
ASSIST_API void assist_engine_destroy(assist_engine* engine);

/* Replaces the contact list. Names the index rejects (empty, too long) are skipped, and the count of those is
 * written to rejected when it is not NULL. */
ASSIST_API assist_status assist_engine_set_contacts(assist_engine* engine, const assist_contact* contacts, size_t count,
                                                    size_t* rejected);

/* Handles one request. On success *json points at a NUL-terminated JSON object and *json_len is its length without
 * the terminator (json_len may be NULL). */
ASSIST_API assist_status assist_engine_handle(assist_engine* engine, const char* utf8, size_t utf8_len, char** json,
                                              size_t* json_len);

ASSIST_API void assist_string_free(char* json);

/* Streaming. Feed the growing transcript with assist_stream_update. Only the newest text is worked on: older text
 * still waiting is dropped, and a result that finishes after newer text arrived is not reported. on_partial is called
 * with the JSON of each partial result and a sequence number that rises with each update. The JSON is valid only
 * during the call. */
typedef void (*assist_partial_fn)(const char* json, size_t json_len, uint64_t sequence, void* user_data);

ASSIST_API assist_status assist_stream_create(const assist_engine* engine, assist_partial_fn on_partial, void* user_data,
                                              assist_stream** out);
ASSIST_API assist_status assist_stream_update(assist_stream* stream, const char* utf8, size_t utf8_len);
/* Waits for the work on the newest text, then returns its result like assist_engine_handle. One use per stream. */
ASSIST_API assist_status assist_stream_finish(assist_stream* stream, char** json, size_t* json_len);
/* Abandons the stream. Nothing more is reported to on_partial after this returns. */
ASSIST_API void assist_stream_cancel(assist_stream* stream);
ASSIST_API void assist_stream_destroy(assist_stream* stream);

#ifdef __cplusplus
}
#endif

#endif /* ASSIST_H */
