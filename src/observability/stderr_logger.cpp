#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "halcyon/observability/logging.hpp"

namespace halcyon::obs {

namespace {

// Appends a string as a safe logfmt token: raw when it has no delimiter- or
// injection-relevant characters, otherwise double-quoted with escaping. Without
// this, a value containing a space (e.g. raw SQL) breaks `key=value` parsing and
// a newline injects a forged log line.
void append_logfmt_string(std::string& out, std::string_view s) {
    bool needsQuote = s.empty();
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || c == '"' || c == '=' || c == '\\') {
            needsQuote = true;
            break;
        }
    }
    if (!needsQuote) {
        out.append(s.data(), s.size());
        return;
    }
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[5];
                    std::snprintf(buf, sizeof(buf), "\\x%02x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void append_value(
    std::string& out,
    const std::variant<std::int64_t, double, bool, std::string_view>& v) {
    std::visit(
        [&out](const auto& x) {
            using X = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<X, std::string_view>)
                append_logfmt_string(out, x);
            else if constexpr (std::is_same_v<X, bool>)
                out += x ? "true" : "false";
            else
                out += std::to_string(x);
        },
        v);
}

}  // namespace

void StderrLogger::log(LogLevel level, std::string_view event,
                       std::initializer_list<LogField> fields) noexcept {
    try {
        std::string line;
        line.reserve(96);
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
        line += "ts=";
        line += std::to_string(secs);
        line += " level=";
        line += to_string(level);
        line += " event=";
        append_logfmt_string(line, event);
        for (const auto& f : fields) {
            line += ' ';
            append_logfmt_string(line, f.key);  // public callers may supply keys
            line += '=';
            append_value(line, f.value);
        }
        line += '\n';
        std::fwrite(line.data(), 1, line.size(), out_);
    } catch (...) {
        // log() is noexcept: allocation failure drops the line, never throws.
    }
}

}  // namespace halcyon::obs
