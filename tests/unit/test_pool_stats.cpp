#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <thread>

#include "halcyon/database.hpp"
#include "halcyon/pool.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Clock;
using halcyon::ConnectionPool;
using halcyon::Database;
using halcyon::PoolConfig;
using halcyon::PoolStats;
using halcyon::testing::MockCliDriver;

namespace {

PoolConfig noThread() {
    PoolConfig c;
    c.startMaintenanceThread = false;
    return c;
}

void expect_invariants(const PoolStats& s) {
    EXPECT_LE(s.busy, s.size);
    EXPECT_EQ(s.idle + s.busy, s.size);
}

}  // namespace

TEST(PoolStats, WarmupAndAcquireReleaseTransitions) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 2;
    cfg.max = 4;
    auto pool = ConnectionPool::create(driver, {"X"}, cfg).value();

    PoolStats s0 = pool->stats();
    expect_invariants(s0);
    EXPECT_EQ(s0.size, 2u);
    EXPECT_EQ(s0.idle, 2u);
    EXPECT_EQ(s0.busy, 0u);
    EXPECT_EQ(s0.createdTotal, 2u);
    EXPECT_EQ(s0.acquiredTotal, 0u);
    EXPECT_EQ(s0.peakBusy, 0u);

    {
        auto a = pool->acquire();
        ASSERT_TRUE(a.ok());
        auto b = pool->acquire();
        ASSERT_TRUE(b.ok());
        auto c = pool->acquire();  // grows past min
        ASSERT_TRUE(c.ok());
        PoolStats s1 = pool->stats();
        expect_invariants(s1);
        EXPECT_EQ(s1.size, 3u);
        EXPECT_EQ(s1.busy, 3u);
        EXPECT_EQ(s1.acquiredTotal, 3u);
        EXPECT_EQ(s1.createdTotal, 3u);
        EXPECT_EQ(s1.peakBusy, 3u);
    }
    PoolStats s2 = pool->stats();
    expect_invariants(s2);
    EXPECT_EQ(s2.busy, 0u);
    EXPECT_EQ(s2.idle, 3u);
    EXPECT_EQ(s2.peakBusy, 3u);  // high-water mark persists
}

TEST(PoolStats, TimeoutAndWaiterCounters) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    cfg.acquireTimeout = std::chrono::milliseconds(50);
    auto pool = ConnectionPool::create(driver, {"X"}, cfg).value();

    auto held = pool->acquire();
    ASSERT_TRUE(held.ok());

    // A blocked acquirer shows up in waiters, then times out.
    auto fut = std::async(std::launch::async, [&] { return pool->acquire(); });
    // Poll for the waiter (bounded).
    bool sawWaiter = false;
    for (int i = 0; i < 200 && !sawWaiter; ++i) {
        sawWaiter = pool->stats().waiters == 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(sawWaiter);
    auto r = fut.get();
    ASSERT_FALSE(r.ok());
    PoolStats s = pool->stats();
    expect_invariants(s);
    EXPECT_EQ(s.waiters, 0u);
    EXPECT_EQ(s.acquireTimeoutsTotal, 1u);
}

TEST(PoolStats, DiscardOnBrokenRelease) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 2;
    auto pool = ConnectionPool::create(driver, {"X"}, cfg).value();
    {
        auto a = pool->acquire();
        ASSERT_TRUE(a.ok());
        a.value().markBroken();
    }
    PoolStats s = pool->stats();
    expect_invariants(s);
    EXPECT_EQ(s.discardedTotal, 1u);
    EXPECT_EQ(s.size, 0u);
}

// A SUCCESSFUL validation reconnect still discards a physical connection: the
// dead original is destroyed in place and replaced. Without counting it,
// createdTotal (which counts the replacement) drifts against
// discardedTotal + size.
TEST(PoolStats, SuccessfulValidationReconnectCountsDiscard) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    cfg.validateOnAcquire = true;
    auto pool = ConnectionPool::create(driver, {"X"}, cfg).value();
    driver.aliveResults.push_back(false);  // validation finds the conn dead
    {
        auto lease = pool->acquire();  // transparent reconnect in place
        ASSERT_TRUE(lease.ok());
    }
    PoolStats s = pool->stats();
    expect_invariants(s);
    EXPECT_EQ(s.size, 1u);
    EXPECT_EQ(s.discardedTotal, 1u);  // the dead original was discarded
    EXPECT_EQ(s.createdTotal, 2u);    // warmup + replacement
}

TEST(PoolStats, ReapCountersSplitIdleAndLifetime) {
    MockCliDriver driver;
    auto now = std::make_shared<Clock::time_point>(Clock::now());
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 4;
    cfg.idleTimeout = std::chrono::milliseconds(1000);
    cfg.maxLifetime = std::chrono::milliseconds(60000);
    cfg.now = [now] { return *now; };
    auto pool = ConnectionPool::create(driver, {"X"}, cfg).value();

    {  // grow to 3, release all
        auto a = pool->acquire().value();
        auto b = pool->acquire().value();
        auto c = pool->acquire().value();
    }
    *now += std::chrono::milliseconds(1500);  // past idleTimeout, below lifetime
    pool->maintain();
    PoolStats s1 = pool->stats();
    expect_invariants(s1);
    EXPECT_EQ(s1.reapedIdleTotal, 2u);  // reaped down to min=1
    EXPECT_EQ(s1.reapedLifetimeTotal, 0u);

    *now += std::chrono::milliseconds(60000);  // past maxLifetime
    pool->maintain();
    PoolStats s2 = pool->stats();
    expect_invariants(s2);
    EXPECT_EQ(s2.reapedLifetimeTotal, 1u);
    // refill back to min recreates
    EXPECT_GE(s2.createdTotal, s1.createdTotal + 1);
}

// v1.2 review P2: peakBusy is a high-water mark, so no snapshot may ever
// report busy > peakBusy. During validate-on-acquire the slot leaves idle_
// (busy++) and the pool mutex is dropped for the isAlive probe; peakBusy must
// already reflect the increase by then. A concurrent snapshot taken inside
// that gap catches a regression deterministically.
TEST(PoolStats, PeakBusyNeverBelowBusyDuringValidation) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    cfg.validateOnAcquire = true;
    auto pool = ConnectionPool::create(driver, {"X"}, cfg).value();
    driver.aliveResults.push_back(true);  // validation succeeds (no reconnect)

    std::promise<void> inValidate, releaseValidate;
    auto inF = inValidate.get_future();
    auto relF = releaseValidate.get_future();
    driver.isAliveHook = [&] {
        inValidate.set_value();
        relF.wait();  // hold the acquirer inside the validation gap
    };

    auto fut = std::async(std::launch::async, [&] { return pool->acquire(); });
    inF.wait();  // acquirer popped the slot (busy=1) and is inside isAlive
    PoolStats s = pool->stats();
    releaseValidate.set_value();
    auto lease = fut.get();
    ASSERT_TRUE(lease.ok());

    expect_invariants(s);
    EXPECT_EQ(s.busy, 1u);
    EXPECT_GE(s.peakBusy, s.busy);  // the invariant the bug violates (was 0)
}

TEST(PoolStats, DatabaseForwarderMatchesPool) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    PoolStats viaDb = db.poolStats();
    PoolStats viaPool = db.pool().stats();
    expect_invariants(viaDb);
    EXPECT_EQ(viaDb.size, viaPool.size);
    EXPECT_EQ(viaDb.createdTotal, viaPool.createdTotal);
}
