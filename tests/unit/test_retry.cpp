#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "halcyon/result.hpp"
#include "halcyon/retry.hpp"

using halcyon::BackoffPolicy;
using halcyon::ErrorCode;
using halcyon::ExecPolicy;
using halcyon::Result;
using halcyon::with_retry;
using namespace std::chrono_literals;

namespace {
halcyon::Error err(ErrorCode code, bool retriable) {
    halcyon::Error e;
    e.code = code;
    e.retriable = retriable;
    e.message = "scripted";
    return e;
}
}  // namespace

TEST(Backoff, ExponentialAndCapped) {
    BackoffPolicy b;
    b.baseDelay = 10ms;
    b.maxDelay = 50ms;
    EXPECT_EQ(b.delay_for(1), 10ms);
    EXPECT_EQ(b.delay_for(2), 20ms);
    EXPECT_EQ(b.delay_for(3), 40ms);
    EXPECT_EQ(b.delay_for(4), 50ms);  // capped
    EXPECT_EQ(b.delay_for(99), 50ms);  // no overflow, still capped
}

TEST(WithRetry, ReturnsImmediatelyOnSuccess) {
    int calls = 0;
    auto policy = ExecPolicy::idempotent(3);
    policy.backoff.sleep = [](std::chrono::milliseconds) {};
    auto r = with_retry(policy, [&]() -> Result<int> {
        ++calls;
        return 42;
    });
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 42);
    EXPECT_EQ(calls, 1);
}

TEST(WithRetry, RetriesRetriableUpToMaxAttempts) {
    int calls = 0;
    std::vector<std::chrono::milliseconds> slept;
    auto policy = ExecPolicy::idempotent(3);
    policy.backoff.baseDelay = 5ms;
    policy.backoff.sleep = [&](std::chrono::milliseconds d) { slept.push_back(d); };
    auto r = with_retry(policy, [&]() -> Result<int> {
        ++calls;
        return err(ErrorCode::Transient, true);
    });
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Transient);
    EXPECT_EQ(calls, 3);            // initial + 2 retries
    EXPECT_EQ(slept.size(), 2u);   // slept before each retry
}

TEST(WithRetry, DoesNotRetryNonRetriable) {
    int calls = 0;
    auto policy = ExecPolicy::idempotent(5);
    policy.backoff.sleep = [](std::chrono::milliseconds) {};
    auto r = with_retry(policy, [&]() -> Result<int> {
        ++calls;
        return err(ErrorCode::Constraint, false);
    });
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(calls, 1);
}

TEST(WithRetry, OncePolicyNeverRetries) {
    int calls = 0;
    auto policy = ExecPolicy::once();
    policy.backoff.sleep = [](std::chrono::milliseconds) {};
    auto r = with_retry(policy, [&]() -> Result<int> {
        ++calls;
        return err(ErrorCode::Transient, true);
    });
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(calls, 1);
}

TEST(IsReadOnly, RecognizesSafeStatements) {
    using halcyon::detail::is_read_only;
    EXPECT_TRUE(is_read_only("SELECT 1 FROM SYSIBM.SYSDUMMY1"));
    EXPECT_TRUE(is_read_only("  \n select id from t"));
    EXPECT_TRUE(is_read_only("WITH x AS (SELECT 1) SELECT * FROM x"));
    EXPECT_TRUE(is_read_only("VALUES (1)"));
    EXPECT_FALSE(is_read_only("INSERT INTO t VALUES (1)"));
    EXPECT_FALSE(is_read_only("UPDATE t SET a = 1"));
    EXPECT_FALSE(is_read_only("DELETE FROM t"));
    EXPECT_FALSE(is_read_only(""));
}
