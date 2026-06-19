#include <gtest/gtest.h>

#include <chrono>

#include "halcyon/pool.hpp"
#include "mock_cli_driver.hpp"

using halcyon::ConnectionPool;
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
    { auto pc = pool->acquire(); ASSERT_TRUE(pc.ok()); }
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
