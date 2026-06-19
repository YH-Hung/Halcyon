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

TEST(IsReadOnly, RequiresWordBoundaryAfterKeyword) {
    using halcyon::detail::is_read_only;
    // Leading characters match a keyword but the token is a different word.
    EXPECT_FALSE(is_read_only("WITHOUT ROWID x"));
    EXPECT_FALSE(is_read_only("SELECTED VALUES FROM t"));
    EXPECT_FALSE(is_read_only("VALUESX (1)"));
    // A keyword terminated by end-of-string or a paren is still a real keyword.
    EXPECT_TRUE(is_read_only("SELECT"));
    EXPECT_TRUE(is_read_only("VALUES(1)"));
}

TEST(IsReadOnly, WithMustResolveToSelectNotDataChange) {
    using halcyon::detail::is_read_only;
    // WITH … SELECT is read-only.
    EXPECT_TRUE(is_read_only("WITH t AS (SELECT 1) SELECT * FROM t"));
    EXPECT_TRUE(is_read_only("with a AS (SELECT 1), b AS (SELECT 2) "
                             "SELECT * FROM a JOIN b ON a.x = b.x"));
    // A data-change verb inside a CTE body (parenthesised / quoted) must not
    // fool the classifier: the main statement is still a SELECT.
    EXPECT_TRUE(is_read_only("WITH t AS (SELECT 'DELETE' FROM s) SELECT * FROM t"));
    // WITH feeding a data-change statement is NOT safe to auto-retry.
    EXPECT_FALSE(is_read_only("WITH t AS (SELECT 1) DELETE FROM x"));
    EXPECT_FALSE(is_read_only("WITH t AS (SELECT 1) INSERT INTO x SELECT * FROM t"));
    EXPECT_FALSE(is_read_only("WITH t AS (SELECT 1) UPDATE x SET a = 1"));
    EXPECT_FALSE(is_read_only("WITH t AS (SELECT 1) MERGE INTO x USING t ON x.id = t.id"));
    EXPECT_FALSE(is_read_only("with t as (select 1) delete from x"));
}
