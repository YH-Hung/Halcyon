#pragma once

#include <cctype>
#include <chrono>
#include <string_view>

#include "halcyon/error.hpp"

namespace halcyon::detail::obs {

// Bounded op label from the leading SQL verb, for the {op} metric/span label.
// Keeps label cardinality small (one of a fixed set) regardless of the SQL text.
inline std::string_view op_label(std::string_view sql) {
    std::size_t i = 0;
    while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i])))
        ++i;
    const std::size_t s = i;
    while (i < sql.size() && std::isalpha(static_cast<unsigned char>(sql[i])))
        ++i;
    const std::string_view tok = sql.substr(s, i - s);
    auto ieq = [](std::string_view a, std::string_view kw) {
        if (a.size() != kw.size()) return false;
        for (std::size_t k = 0; k < kw.size(); ++k)
            if (std::toupper(static_cast<unsigned char>(a[k])) != kw[k])
                return false;
        return true;
    };
    if (ieq(tok, "SELECT")) return "select";
    if (ieq(tok, "INSERT")) return "insert";
    if (ieq(tok, "UPDATE")) return "update";
    if (ieq(tok, "DELETE")) return "delete";
    if (ieq(tok, "MERGE")) return "merge";
    if (ieq(tok, "WITH")) return "with";
    if (ieq(tok, "VALUES")) return "values";
    return "other";
}

// Lowercased, fixed ErrorCode label for errors_total{code}. Stable storage, no
// allocation — keeps label cardinality bounded to the ErrorCode enum.
inline std::string_view code_label(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok:
            return "ok";
        case ErrorCode::Connection:
            return "connection";
        case ErrorCode::Timeout:
            return "timeout";
        case ErrorCode::Constraint:
            return "constraint";
        case ErrorCode::Syntax:
            return "syntax";
        case ErrorCode::Deadlock:
            return "deadlock";
        case ErrorCode::Transient:
            return "transient";
        case ErrorCode::Mapping:
            return "mapping";
        case ErrorCode::Pool:
            return "pool";
        case ErrorCode::InvalidArgument:
            return "invalid_argument";
        case ErrorCode::InvalidState:
            return "invalid_state";
        case ErrorCode::Unknown:
            return "unknown";
    }
    return "unknown";
}

// Monotonic wall-time sampler for duration histograms.
struct Timer {
    std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    double elapsed_seconds() const {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - start)
            .count();
    }
};

}  // namespace halcyon::detail::obs
