#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

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

TEST(PoolConcurrency, AcquireDoesNotHoldLockDuringConnect) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 2;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();  // warmup connect

    // Hold the one warmed connection so the next acquire must grow → connect.
    auto held = pool->acquire();
    ASSERT_TRUE(held.ok());

    std::promise<void> connectStarted;
    std::promise<void> releaseConnect;
    auto startedFut = connectStarted.get_future();
    auto releaseFut = releaseConnect.get_future().share();
    driver.connectHook = [&] {
        connectStarted.set_value();
        releaseFut.wait();  // block mid-connect until the test releases us
    };

    std::thread grower([&] {
        auto pc = pool->acquire();  // grows the pool: connects (and blocks)
        (void)pc;                   // released at thread exit
    });
    startedFut.wait();  // grower is now blocked inside connect()

    // While the grower is mid-connect, an unrelated pool operation must not block
    // on mu_. If the pool held its mutex across connect, this would hang.
    auto idleFut =
        std::async(std::launch::async, [&] { return pool->idle_count(); });
    auto status = idleFut.wait_for(std::chrono::seconds(2));

    releaseConnect.set_value();  // let the grower finish before any assertion
    grower.join();
    auto idleVal = idleFut.get();

    EXPECT_EQ(status, std::future_status::ready)
        << "pool mutex was held during connect (idle_count blocked)";
    EXPECT_EQ(idleVal, 0u);
}

TEST(PoolConcurrency, MaintainDoesNotHoldLockDuringConnect) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 2;
    cfg.max = 2;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();  // warms 2
    ASSERT_EQ(pool->total_count(), 2u);

    // Discard one connection so the next maintain() pass must refill up to min.
    {
        auto pc = pool->acquire();
        ASSERT_TRUE(pc.ok());
        pc.value().markBroken();
    }
    ASSERT_EQ(pool->total_count(), 1u);

    std::promise<void> connectStarted;
    std::promise<void> releaseConnect;
    auto startedFut = connectStarted.get_future();
    auto releaseFut = releaseConnect.get_future().share();
    driver.connectHook = [&] {
        connectStarted.set_value();
        releaseFut.wait();  // block mid-connect until the test releases us
    };

    std::thread mt([&] { pool->maintain(); });  // reaps, then refills (connects)
    startedFut.wait();                          // maintain() is now blocked inside the refill connect

    // While the refill is mid-connect, an unrelated pool operation must not block
    // on mu_. If maintain() held its mutex across connect, this would hang.
    auto fut =
        std::async(std::launch::async, [&] { return pool->total_count(); });
    auto status = fut.wait_for(std::chrono::seconds(2));

    releaseConnect.set_value();  // let the refill finish before any assertion
    mt.join();
    auto val = fut.get();

    EXPECT_EQ(status, std::future_status::ready)
        << "pool mutex was held during maintain() connect";
    EXPECT_EQ(val, 1u);                  // refill not yet committed when sampled mid-connect
    EXPECT_EQ(pool->total_count(), 2u);  // refill completed
}

TEST(PoolConcurrency, MaintainRefillFailureWakesAcquireWaiter) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    cfg.acquireTimeout = 5s;      // large, so a missed wakeup is observable
    cfg.backoff.maxAttempts = 1;  // a single failed connect attempt is terminal
    cfg.backoff.sleep = [](std::chrono::milliseconds) {};
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();  // warms 1

    // Empty the single slot so the pool must grow/refill again.
    {
        auto pc = pool->acquire();
        ASSERT_TRUE(pc.ok());
        pc.value().markBroken();
    }
    ASSERT_EQ(pool->total_count(), 0u);

    std::promise<void> connectStarted;
    std::promise<void> releaseConnect;
    auto startedFut = connectStarted.get_future();
    auto releaseFut = releaseConnect.get_future().share();
    std::atomic<bool> firstConnect{true};
    driver.connectHook = [&] {
        if (firstConnect.exchange(false)) {
            connectStarted.set_value();
            releaseFut.wait();  // block maintain()'s refill connect
        }
    };
    Error e;
    e.code = ErrorCode::Connection;
    e.retriable = true;
    driver.connectErrors.push_back(e);  // maintain()'s refill connect fails

    std::thread mt([&] { pool->maintain(); });  // reserves pending_, connect blocks
    startedFut.wait();                          // pending_ == 1, maintain parked inside connect

    // An acquirer now blocks because slots_(0) + pending_(1) >= max(1).
    auto acq = std::async(std::launch::async, [&] { return pool->acquire(); });
    std::this_thread::sleep_for(200ms);  // let the acquirer park on cv_

    releaseConnect.set_value();  // maintain()'s connect fails → pending_ drops

    // With the fix, the freed capacity wakes the acquirer at once; without it the
    // acquirer sleeps until acquireTimeout (5s).
    auto status = acq.wait_for(2s);
    mt.join();
    auto lease = acq.get();

    EXPECT_EQ(status, std::future_status::ready)
        << "acquire waiter not woken after maintain() refill failure";
    EXPECT_TRUE(lease.ok());
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
