// Turning a Result into JSON, for the C interface and the command-line tool.
//
// Everything the engine returns can contain text that came from the user, and JSON has to be valid UTF-8, so the
// writer checks every string against the Unicode definition of well-formed UTF-8 (no overlong forms, no surrogates,
// nothing above U+10FFFF) and replaces each bad byte with U+FFFD instead of passing it on. Control characters are
// escaped. Whatever bytes go in, what comes out parses.
#pragma once

#include <string>
#include <string_view>

#include "assist/engine.hpp"

namespace assist {

// Append `text` to `out` as a quoted JSON string.
void append_json_string(std::string& out, std::string_view text);

std::string to_json(const Result& result);

}  // namespace assist
