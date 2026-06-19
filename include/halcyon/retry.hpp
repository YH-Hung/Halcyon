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
        return scaled > maxDelay ? maxDelay : scaled;
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
// Known limitation: a SELECT whose body performs a write via a Db2 data-change
// table reference (e.g. SELECT … FROM FINAL TABLE(INSERT …)) is classified
// read-only here; the facade should not mark such statements idempotent.
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

    std::size_t i = 0;
    while (i < sql.size() &&
           std::isspace(static_cast<unsigned char>(sql[i]))) {
        ++i;
    }
    const std::size_t kw_start = i;
    while (i < sql.size() && is_ident(static_cast<unsigned char>(sql[i]))) ++i;
    const std::string_view lead = sql.substr(kw_start, i - kw_start);

    if (ieq(lead, "SELECT") || ieq(lead, "VALUES")) return true;
    if (!ieq(lead, "WITH")) return false;

    // WITH: the main statement's verb is the first depth-0 keyword token; CTE
    // bodies are parenthesised (depth >= 1), so their inner SELECTs are skipped.
    // Quoted strings and delimited identifiers are skipped so their contents
    // (and any parens within) cannot perturb the scan.
    int depth = 0;
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
        if (depth == 0 && is_ident(static_cast<unsigned char>(ch))) {
            const std::size_t s = i;
            while (i < sql.size() && is_ident(static_cast<unsigned char>(sql[i])))
                ++i;
            const std::string_view tok = sql.substr(s, i - s);
            if (ieq(tok, "SELECT")) return true;
            if (ieq(tok, "INSERT") || ieq(tok, "UPDATE") || ieq(tok, "DELETE") ||
                ieq(tok, "MERGE"))
                return false;
            continue;  // CTE name / AS / other keyword — keep scanning
        }
        ++i;
    }
    return false;  // no resolvable main SELECT → not safe to auto-retry
}

}  // namespace detail

}  // namespace halcyon
