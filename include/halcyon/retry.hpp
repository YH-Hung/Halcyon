#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <string_view>
#include <thread>
#include <utility>

namespace halcyon {

// Exponential backoff with a hard cap and an injectable sleep (so tests run
// instantly and the reconnect loop can be driven deterministically).
struct BackoffPolicy {
    std::chrono::milliseconds baseDelay{50};
    std::chrono::milliseconds maxDelay{2000};
    int maxAttempts = 3;
    std::function<void(std::chrono::milliseconds)> sleep =
        [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); };

    // delay for a 1-based attempt index: baseDelay * 2^(attempt-1), capped.
    std::chrono::milliseconds delay_for(int attempt) const {
        if (attempt < 1) attempt = 1;
        long long mult = 1;
        for (int i = 1; i < attempt && mult < (1LL << 30); ++i) mult <<= 1;
        auto scaled = baseDelay * mult;
        if (scaled > maxDelay) return maxDelay;
        return std::chrono::duration_cast<std::chrono::milliseconds>(scaled);
    }
};

// Per-call retry policy. Idempotency is expressed by allowing > 1 attempt; the
// caller (or pool/facade) decides which statements are safe to replay.
struct ExecPolicy {
    int maxAttempts = 1;
    BackoffPolicy backoff{};

    static ExecPolicy once() { return ExecPolicy{1, {}}; }
    static ExecPolicy idempotent(int attempts = 3) {
        ExecPolicy p;
        p.maxAttempts = attempts < 1 ? 1 : attempts;
        return p;
    }
};

// Calls fn() (returns Result<T>); while the result is an error that is retriable
// and attempts remain, sleeps per backoff and retries. Returns the last result.
template <class Fn>
auto with_retry(const ExecPolicy& policy, Fn&& fn) {
    auto r = fn();
    for (int attempt = 1; attempt < policy.maxAttempts; ++attempt) {
        if (r.ok() || !r.error().retriable) return r;
        policy.backoff.sleep(policy.backoff.delay_for(attempt));
        r = fn();
    }
    return r;
}

namespace detail {

// True for statements safe to auto-retry: read-only SELECT / WITH…SELECT /
// VALUES. The leading keyword must be a whole token (so WITHOUT / SELECTED /
// VALUESX are rejected), and a WITH must resolve to a SELECT — a WITH that feeds
// a data-change statement (WITH … INSERT/UPDATE/DELETE/MERGE) is NOT read-only
// and must not be replayed.
//
// A data-change verb (INSERT/UPDATE/DELETE/MERGE) appearing as a whole token at
// ANY parenthesis depth also disqualifies the statement, so a Db2 data-change
// table reference — SELECT … FROM FINAL/NEW/OLD TABLE(INSERT/UPDATE/DELETE/MERGE …),
// whose embedded write lives inside parentheses — is correctly classified as NOT
// read-only even though it leads with SELECT. Quoted strings and delimited
// identifiers are skipped so their contents cannot perturb the scan.
inline bool is_read_only(std::string_view sql) noexcept {
    auto is_ident = [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_';
    };
    auto ieq = [](std::string_view a, std::string_view kw) {
        if (a.size() != kw.size()) return false;
        for (std::size_t k = 0; k < kw.size(); ++k) {
            if (std::toupper(static_cast<unsigned char>(a[k])) != kw[k])
                return false;
        }
        return true;
    };
    auto is_data_change = [&](std::string_view t) {
        return ieq(t, "INSERT") || ieq(t, "UPDATE") || ieq(t, "DELETE") ||
               ieq(t, "MERGE");
    };

    std::size_t i = 0;
    while (i < sql.size() &&
           std::isspace(static_cast<unsigned char>(sql[i]))) {
        ++i;
    }
    const std::size_t kw_start = i;
    while (i < sql.size() && is_ident(static_cast<unsigned char>(sql[i]))) ++i;
    const std::string_view lead = sql.substr(kw_start, i - kw_start);

    const bool lead_select = ieq(lead, "SELECT") || ieq(lead, "VALUES");
    const bool lead_with = ieq(lead, "WITH");
    if (!lead_select && !lead_with) return false;

    // Scan the remainder. A data-change verb at ANY depth means the statement
    // writes (e.g. FROM FINAL TABLE(INSERT …) nests the verb in parens) → not
    // retryable. For a leading WITH, the main statement's verb is the first
    // depth-0 keyword token (CTE bodies are parenthesised at depth >= 1) and must
    // resolve to SELECT. Quoted strings / delimited identifiers are skipped.
    int depth = 0;
    bool with_main_resolved = false;
    bool with_main_select = false;
    while (i < sql.size()) {
        const char ch = sql[i];
        if (ch == '\'' || ch == '"') {
            const char quote = ch;
            ++i;
            while (i < sql.size()) {
                if (sql[i] == quote) {
                    if (i + 1 < sql.size() && sql[i + 1] == quote) {
                        i += 2;  // doubled quote = escaped, stay in the literal
                        continue;
                    }
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }
        if (ch == '(') {
            ++depth;
            ++i;
            continue;
        }
        if (ch == ')') {
            if (depth > 0) --depth;
            ++i;
            continue;
        }
        if (is_ident(static_cast<unsigned char>(ch))) {
            const std::size_t s = i;
            while (i < sql.size() && is_ident(static_cast<unsigned char>(sql[i])))
                ++i;
            const std::string_view tok = sql.substr(s, i - s);
            if (is_data_change(tok)) return false;  // write at any depth
            if (lead_with && depth == 0 && !with_main_resolved &&
                ieq(tok, "SELECT")) {
                with_main_select = true;  // main statement is a SELECT
                with_main_resolved = true;
            }
            continue;
        }
        ++i;
    }
    // Leading SELECT/VALUES with no embedded write is safe; a WITH is safe only
    // if its main statement resolved to a SELECT.
    return lead_with ? with_main_select : true;
}

}  // namespace detail

}  // namespace halcyon
