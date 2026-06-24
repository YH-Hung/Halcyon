#include "halcyon/detail/cli/sqlstate.hpp"

#include <algorithm>

namespace halcyon::detail::cli {

namespace {
bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), s.begin());
}
}  // namespace

Classification classify_sqlstate(std::string_view sqlstate,
                                 int nativeError) noexcept {
    if (starts_with(sqlstate, "08")) return {ErrorCode::Connection, true};
    if (nativeError == -30081) return {ErrorCode::Transient, true};
    if (sqlstate == "40001") return {ErrorCode::Deadlock, true};
    if (sqlstate == "57033" || starts_with(sqlstate, "57")) {
        // 57033 lock timeout / resource-not-available class: treat as retriable
        // timeout.
        return {ErrorCode::Timeout, true};
    }
    if (starts_with(sqlstate, "23")) return {ErrorCode::Constraint, false};
    if (starts_with(sqlstate, "42")) return {ErrorCode::Syntax, false};
    return {ErrorCode::Unknown, false};
}

}  // namespace halcyon::detail::cli
