#include <gtest/gtest.h>

#include <chrono>
#include <memory>

#include "halcyon/pool.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Clock;
using halcyon::ConnectionPool;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;
using namespace std::chrono_literals;

namespace {
struct FakeClock {
    std::shared_ptr<Clock::time_point> now =
        std::make_shared<Clock::time_point>(Clock::now());
    void advance(std::chrono::milliseconds d) { *now += d; }
    std::function<Clock::time_point()> fn() const {
        auto n = now;
        return [n] { return *n; };
    }
};
}  // namespace

TEST(PoolMaintenance, ReapsIdleBeyondTimeoutDownToMin) {
    MockCliDriver driver;
    FakeClock clk;
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    cfg.min = 1;
    cfg.max = 4;
    cfg.idleTimeout = 100ms;
    cfg.now = clk.fn();
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();

    { auto a = pool->acquire(); auto b = pool->acquire(); auto c = pool->acquire(); }
    EXPECT_EQ(pool->idle_count(), 3u);

    clk.advance(200ms);
    pool->maintain();
    EXPECT_EQ(pool->total_count(), 1u);  // reaped down to min
    EXPECT_EQ(pool->idle_count(), 1u);
}

TEST(PoolMaintenance, DoesNotReapBelowMin) {
    MockCliDriver driver;
    FakeClock clk;
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    cfg.min = 2;
    cfg.idleTimeout = 100ms;
    cfg.now = clk.fn();
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    EXPECT_EQ(pool->idle_count(), 2u);

    clk.advance(500ms);
    pool->maintain();
    EXPECT_EQ(pool->total_count(), 2u);  // never below min
}

TEST(PoolMaintenance, ReapsBeyondMaxLifetimeEvenAtMin) {
    MockCliDriver driver;
    FakeClock clk;
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    cfg.min = 1;
    cfg.maxLifetime = 100ms;
    cfg.idleTimeout = 1h;  // not the trigger here
    cfg.now = clk.fn();
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    EXPECT_EQ(driver.connectCalls, 1);

    clk.advance(200ms);
    pool->maintain();
    EXPECT_EQ(pool->total_count(), 1u);   // replaced, not dropped
    EXPECT_EQ(driver.connectCalls, 2);    // old reaped + fresh refill
    EXPECT_EQ(driver.disconnectCalls, 1);
}

TEST(PoolMaintenance, RefillsToMinAfterBrokenRemoval) {
    MockCliDriver driver;
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    cfg.min = 2;
    cfg.max = 4;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    {
        auto a = pool->acquire();
        auto b = pool->acquire();
        a.value().markBroken();
    }
    EXPECT_EQ(pool->total_count(), 1u);  // one discarded
    pool->maintain();
    EXPECT_EQ(pool->total_count(), 2u);  // refilled to min
}
