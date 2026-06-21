#include <gtest/gtest.h>

#include <chrono>

#include "halcyon/pool.hpp"
#include "mock_cli_driver.hpp"

using halcyon::ConnectionPool;
using halcyon::Error;
using halcyon::ErrorCode;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;
using namespace std::chrono_literals;

namespace {
PoolConfig noThread(PoolConfig cfg = {}) {
    cfg.startMaintenanceThread = false;
    return cfg;
}
}  // namespace

TEST(PoolBasics, WarmsUpToMinOnCreate) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 2;
    cfg.max = 4;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg);
    ASSERT_TRUE(pool.ok()) << pool.error().message;
    EXPECT_EQ(pool.value()->total_count(), 2u);
    EXPECT_EQ(pool.value()->idle_count(), 2u);
    EXPECT_EQ(driver.connectCalls, 2);
}

TEST(PoolBasics, AcquireAndReleaseRoundTrips) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 2;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    {
        auto pc = pool->acquire();
        ASSERT_TRUE(pc.ok());
        EXPECT_EQ(pool->idle_count(), 0u);
        EXPECT_EQ(pool->active_count(), 1u);
    }
    EXPECT_EQ(pool->idle_count(), 1u);
    EXPECT_EQ(pool->active_count(), 0u);
}

TEST(PoolBasics, LazyGrowthUpToMax) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 3;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    EXPECT_EQ(driver.connectCalls, 1);  // only min warmed

    auto a = pool->acquire();
    auto b = pool->acquire();
    auto c = pool->acquire();
    ASSERT_TRUE(a.ok() && b.ok() && c.ok());
    EXPECT_EQ(pool->total_count(), 3u);
    EXPECT_EQ(driver.connectCalls, 3);  // grew to max
}

TEST(PoolBasics, AcquireTimesOutWhenExhausted) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    cfg.acquireTimeout = 30ms;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();

    auto held = pool->acquire();
    ASSERT_TRUE(held.ok());
    auto blocked = pool->acquire();
    ASSERT_FALSE(blocked.ok());
    EXPECT_EQ(blocked.error().code, halcyon::ErrorCode::Pool);
}

TEST(PoolBasics, ReleasedConnectionIsReused) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 2;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    {
        auto pc = pool->acquire();
        ASSERT_TRUE(pc.ok());
    }
    auto pc2 = pool->acquire();
    ASSERT_TRUE(pc2.ok());
    EXPECT_EQ(driver.connectCalls, 1);  // reused, not reconnected
}

TEST(PoolBasics, PooledConnectionRunsQueries) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"}, {{halcyon::detail::cli::Value{std::int64_t{7}}}}});
    auto pool = ConnectionPool::create(driver, {"x"}, noThread()).value();

    auto pc = pool->acquire();
    ASSERT_TRUE(pc.ok());
    auto rs = pc.value()->query("SELECT id FROM t");
    ASSERT_TRUE(rs.ok());
    long sum = 0;
    for (auto& row : rs.value()) sum += std::get<0>(row.as<int>());
    EXPECT_EQ(sum, 7);
}

TEST(PoolValidation, ReconnectsDeadConnectionOnAcquire) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 2;
    cfg.validateOnAcquire = true;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    EXPECT_EQ(driver.connectCalls, 1);

    driver.aliveResults.push_back(false);  // next validation fails → reconnect
    auto pc = pool->acquire();
    ASSERT_TRUE(pc.ok());
    EXPECT_EQ(driver.connectCalls, 2);     // reconnected in place
    EXPECT_EQ(driver.disconnectCalls, 1);  // old physical handle dropped
    EXPECT_EQ(pool->total_count(), 1u);    // still one logical slot
}

TEST(PoolValidation, HealthyConnectionIsNotReconnected) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.validateOnAcquire = true;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    driver.aliveResults.push_back(true);
    auto pc = pool->acquire();
    ASSERT_TRUE(pc.ok());
    EXPECT_EQ(driver.connectCalls, 1);
}

TEST(PoolBroken, BrokenConnectionIsDiscardedNotReturned) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 0;
    cfg.max = 2;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    {
        auto pc = pool->acquire();
        ASSERT_TRUE(pc.ok());
        pc.value().markBroken();
        EXPECT_EQ(pool->total_count(), 1u);
    }
    EXPECT_EQ(pool->total_count(), 0u);  // discarded on return
    EXPECT_EQ(pool->idle_count(), 0u);
}

TEST(PoolReconnect, RetriesConnectWithBackoff) {
    MockCliDriver driver;
    Error e;
    e.code = ErrorCode::Connection;
    e.sqlstate = "08001";
    e.retriable = true;
    driver.connectErrors.push_back(e);  // fail once
    driver.connectErrors.push_back(e);  // fail twice, then succeed

    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.backoff.maxAttempts = 3;
    cfg.backoff.sleep = [](std::chrono::milliseconds) {};  // no real waiting
    auto pool = ConnectionPool::create(driver, {"x"}, cfg);
    ASSERT_TRUE(pool.ok()) << pool.error().message;
    EXPECT_EQ(driver.connectCalls, 3);  // 2 failures + 1 success
    EXPECT_EQ(pool.value()->total_count(), 1u);
}

TEST(PoolReconnect, ReturnsErrorWhenAllAttemptsFail) {
    MockCliDriver driver;
    Error e;
    e.code = ErrorCode::Connection;
    e.retriable = true;
    for (int i = 0; i < 5; ++i) driver.connectErrors.push_back(e);

    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.backoff.maxAttempts = 2;
    cfg.backoff.sleep = [](std::chrono::milliseconds) {};
    auto pool = ConnectionPool::create(driver, {"x"}, cfg);
    ASSERT_FALSE(pool.ok());
    EXPECT_EQ(pool.error().code, ErrorCode::Connection);
}

TEST(PoolStatementCache, ReusesPreparedStatementAcrossAcquires) {
    halcyon::testing::MockCliDriver driver;
    driver.resultSets.push_back({{"id"}, {}});
    driver.resultSets.push_back({{"id"}, {}});
    halcyon::PoolConfig cfg;
    cfg.min = 1;
    cfg.max = 1;  // single physical connection, reused
    cfg.startMaintenanceThread = false;
    cfg.statementCacheSize = 8;
    auto pool =
        halcyon::ConnectionPool::create(driver, {"dsn"}, cfg).value();

    {
        auto lease = pool->acquire().value();
        auto r = lease->query("SELECT id FROM t", 1);
        ASSERT_TRUE(r.ok());
    }  // returned to pool; cache warm
    {
        auto lease = pool->acquire().value();
        auto r = lease->query("SELECT id FROM t", 1);
        ASSERT_TRUE(r.ok());
    }

    EXPECT_EQ(driver.preparedSql.size(), 1u);  // same connection -> reuse
}

TEST(PoolStatementCache, ReconnectStartsWithFreshCache) {
    halcyon::testing::MockCliDriver driver;
    driver.resultSets.push_back({{"id"}, {}});
    driver.resultSets.push_back({{"id"}, {}});
    halcyon::PoolConfig cfg;
    cfg.min = 1;
    cfg.max = 1;
    cfg.startMaintenanceThread = false;
    cfg.validateOnAcquire = true;  // validate (isAlive) on each acquire
    cfg.statementCacheSize = 8;
    auto pool =
        halcyon::ConnectionPool::create(driver, {"dsn"}, cfg).value();

    {
        auto lease = pool->acquire().value();
        auto r = lease->query("SELECT id FROM t", 1);  // prepare #1
        ASSERT_TRUE(r.ok());
    }
    driver.aliveResults.push_back(false);  // next acquire sees a dead connection
    {
        auto lease = pool->acquire().value();          // reconnect -> new Connection
        auto r = lease->query("SELECT id FROM t", 1);  // prepare #2 (fresh cache)
        ASSERT_TRUE(r.ok());
    }

    EXPECT_EQ(driver.preparedSql.size(), 2u);  // cache did not survive reconnect
}
