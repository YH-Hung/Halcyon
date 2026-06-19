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

// True for statements safe to auto-retry: read-only SELECT / WITH…SELECT / VALUES.
inline bool is_read_only(std::string_view sql) noexcept {
    std::size_t i = 0;
    while (i < sql.size() &&
           std::isspace(static_cast<unsigned char>(sql[i]))) {
        ++i;
    }
    auto starts = [&](std::string_view kw) {
        if (sql.size() - i < kw.size()) return false;
        for (std::size_t k = 0; k < kw.size(); ++k) {
            if (std::toupper(static_cast<unsigned char>(sql[i + k])) != kw[k])
                return false;
        }
        return true;
    };
    return starts("SELECT") || starts("WITH") || starts("VALUES");
}

}  // namespace detail

}  // namespace halcyon
