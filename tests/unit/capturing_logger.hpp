#pragma once

#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "halcyon/observability/logging.hpp"

namespace halcyon::testing {

struct CapturedEvent {
    obs::LogLevel level;
    std::string event;
    std::vector<std::pair<std::string, std::string>> fields;
};

// Thread-safe capturing ILogger for unit tests. Copies every field into owned
// strings (the LogField contract: values die when log() returns).
class CapturingLogger final : public obs::ILogger {
public:
    void log(obs::LogLevel level, std::string_view event,
             std::initializer_list<obs::LogField> fields) noexcept override {
        CapturedEvent e;
        e.level = level;
        e.event.assign(event.data(), event.size());
        for (const auto& f : fields) {
            std::string v = std::visit(
                [](const auto& x) -> std::string {
                    using X = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<X, std::string_view>)
                        return std::string(x);
                    else if constexpr (std::is_same_v<X, bool>)
                        return x ? "true" : "false";
                    else
                        return std::to_string(x);
                },
                f.value);
            e.fields.emplace_back(std::string(f.key), std::move(v));
        }
        std::lock_guard<std::mutex> lk(mu_);
        events.push_back(std::move(e));
    }

    std::size_t count(std::string_view event) {
        std::lock_guard<std::mutex> lk(mu_);
        std::size_t n = 0;
        for (const auto& e : events)
            if (e.event == event) ++n;
        return n;
    }

    std::vector<CapturedEvent> snapshot() {
        std::lock_guard<std::mutex> lk(mu_);
        return events;
    }

    std::vector<CapturedEvent> events;  // guard with mu_ when threads involved

private:
    std::mutex mu_;
};

}  // namespace halcyon::testing
