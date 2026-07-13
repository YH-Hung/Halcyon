#include <chrono>
#include <cstdio>
#include <string>
#include <type_traits>
#include <variant>

#include "halcyon/observability/logging.hpp"

namespace halcyon::obs {

namespace {

void append_value(
    std::string& out,
    const std::variant<std::int64_t, double, bool, std::string_view>& v) {
    std::visit(
        [&out](const auto& x) {
            using X = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<X, std::string_view>)
                out.append(x.data(), x.size());
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
        line.append(event.data(), event.size());
        for (const auto& f : fields) {
            line += ' ';
            line.append(f.key.data(), f.key.size());
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
