/* Compiled as C (not C++) with warnings as errors, to prove assist.h really is a C header. */
#include "assist.h"

int assist_c_header_check(void) {
    assist_options options = {0};
    options.struct_size = (uint32_t)sizeof(assist_options);
    assist_engine* engine = 0;
    assist_stream* stream = 0;
    char* json = 0;
    size_t length = 0;
    (void)engine;
    (void)stream;
    (void)json;
    (void)length;
    return (int)ASSIST_OK + (int)sizeof(options) - (int)sizeof(options);
}
