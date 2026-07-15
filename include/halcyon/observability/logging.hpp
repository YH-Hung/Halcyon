#pragma once

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string_view>
#include <type_traits>
#include <variant>

namespace halcyon::obs {

enum class LogLevel { Debug,
                      Info,
                      Warn,
                      Error };

inline const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
    }
    return "unknown";
}

// One structured key/value pair attached to a log event. The constructors
// funnel every integral type through int64 explicitly because the C++17
// variant converting constructor would be ambiguous for e.g. `unsigned long`.
struct LogField {
    std::string_view key;
    std::variant<std::int64_t, double, bool, std::string_view> value;

    template <class T, std::enable_if_t<std::is_integral_v<T> &&
                                            !std::is_same_v<T, bool>,
                                        int> = 0>
    LogField(std::string_view k, T v)
        : key(k), value(static_cast<std::int64_t>(v)) {}
    LogField(std::string_view k, double v) : key(k), value(v) {}
    LogField(std::string_view k, bool v) : key(k), value(v) {}
    LogField(std::string_view k, std::string_view v) : key(k), value(v) {}
    LogField(std::string_view k, const char* v)
        : key(k), value(std::string_view(v)) {}
};

/// \brief Sink for structured library log events (spec §3).
///
/// Called synchronously from library paths, always behind a null check, so
/// implementations must be cheap and must not throw. Field values (including
/// string_views) are only valid for the duration of the call — an adapter
/// that retains them must copy.
class ILogger {
public:
    virtual ~ILogger() = default;
    virtual void log(LogLevel level, std::string_view event,
                     std::initializer_list<LogField> fields) noexcept = 0;
};

/// \brief Dependency-free reference adapter: one logfmt line per event
/// (`ts=<epoch-secs> level=<lvl> event=<name> k=v ...`) written with a single
/// fwrite so concurrent lines don't interleave.
class StderrLogger final : public ILogger {
public:
    explicit StderrLogger(std::FILE* out = stderr) : out_(out) {}
    void log(LogLevel level, std::string_view event,
             std::initializer_list<LogField> fields) noexcept override;

private:
    std::FILE* out_;
};

}  // namespace halcyon::obs
