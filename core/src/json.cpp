#include "assist/json.hpp"

#include <cstdint>
#include <cstdio>

namespace assist {
namespace {

// The number of bytes in the well-formed UTF-8 sequence starting at s[i], or 0 if there is none. The ranges are
// Table 3-7 of the Unicode standard.
std::size_t utf8_length(std::string_view s, std::size_t i) noexcept {
    const auto b0 = static_cast<unsigned char>(s[i]);
    auto cont = [&](std::size_t k, unsigned char lo = 0x80, unsigned char hi = 0xBF) {
        if (i + k >= s.size()) return false;
        const auto b = static_cast<unsigned char>(s[i + k]);
        return b >= lo && b <= hi;
    };
    if (b0 < 0x80) return 1;
    if (b0 >= 0xC2 && b0 <= 0xDF) return cont(1) ? 2 : 0;
    if (b0 == 0xE0) return cont(1, 0xA0, 0xBF) && cont(2) ? 3 : 0;
    if (b0 >= 0xE1 && b0 <= 0xEC) return cont(1) && cont(2) ? 3 : 0;
    if (b0 == 0xED) return cont(1, 0x80, 0x9F) && cont(2) ? 3 : 0;  // not a surrogate
    if (b0 >= 0xEE && b0 <= 0xEF) return cont(1) && cont(2) ? 3 : 0;
    if (b0 == 0xF0) return cont(1, 0x90, 0xBF) && cont(2) && cont(3) ? 4 : 0;
    if (b0 >= 0xF1 && b0 <= 0xF3) return cont(1) && cont(2) && cont(3) ? 4 : 0;
    if (b0 == 0xF4) return cont(1, 0x80, 0x8F) && cont(2) && cont(3) ? 4 : 0;  // not above U+10FFFF
    return 0;
}

}  // namespace

void append_json_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof buf, "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
            }
            ++i;
            continue;
        }
        if (const std::size_t n = utf8_length(text, i)) {
            out.append(text.substr(i, n));
            i += n;
        } else {
            out += "\\ufffd";  // one replacement per bad byte
            ++i;
        }
    }
    out.push_back('"');
}

std::string to_json(const Result& r) {
    std::string out = "{";
    auto field = [&](const char* name, std::string_view value) {
        if (out.size() > 1) out.push_back(',');
        append_json_string(out, name);
        out.push_back(':');
        append_json_string(out, value);
    };
    auto number = [&](const char* name, float value) {
        if (out.size() > 1) out.push_back(',');
        append_json_string(out, name);
        char buf[32];
        std::snprintf(buf, sizeof buf, ":%.4f", static_cast<double>(value));
        for (char& c : buf) {
            if (c == ',') c = '.';  // a locale with a decimal comma must not break the JSON
            if (c == 0) break;
        }
        out += buf;
    };
    field("route", to_string(r.route));
    field("intent", r.intent);
    number("confidence", r.confidence);
    number("margin", r.margin);
    field("action", r.action);
    field("reply", r.reply);
    field("forward_text", r.forward_text);
    field("reason", r.reason);
    out += ",\"slots\":{";
    for (std::size_t i = 0; i < r.slots.size(); ++i) {
        if (i > 0) out.push_back(',');
        append_json_string(out, r.slots[i].name);
        out.push_back(':');
        append_json_string(out, r.slots[i].value);
    }
    out += "}}";
    return out;
}

}  // namespace assist
