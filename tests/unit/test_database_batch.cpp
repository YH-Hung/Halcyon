#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "halcyon/database.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;

namespace {
PoolConfig noThread() {
    PoolConfig c;
    c.startMaintenanceThread = false;
    return c;
}
}  // namespace

struct Pair {
    std::int64_t a;
    std::string b;
};
HALCYON_REFLECT(Pair, a, b);

TEST(Batch, ExecutesOncePerRowReturningTotal) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    driver.execRowCounts.push_back(1);
    driver.execRowCounts.push_back(1);
    auto db = Database::open(driver, "X", noThread()).value();

    auto batch = halcyon::batchOf<Pair>({{1, "a"}, {2, "b"}, {3, "c"}});
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 3);
    // one prepare reused across rows
    EXPECT_EQ(driver.preparedSql.size(), 1u);
}

TEST(Batch, EmptyBatchIsNoOp) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", halcyon::Batch{});
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 0);
    EXPECT_EQ(driver.preparedSql.size(), 0u);
}

TEST(Batch, StopsAndReturnsErrorOnRowFailure) {
    MockCliDriver driver;
    halcyon::Error e;
    e.code = halcyon::ErrorCode::Constraint;
    e.message = "dup";
    driver.executeErrors.push_back(e);  // first execute fails
    auto db = Database::open(driver, "X", noThread()).value();

    auto batch = halcyon::batchOf<Pair>({{1, "a"}, {2, "b"}});
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, halcyon::ErrorCode::Constraint);
}

TEST(Batch, ConnectionErrorDiscardsConnection) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    auto db = Database::open(driver, "X", cfg).value();
    ASSERT_EQ(driver.connectCalls, 1);

    halcyon::Error ce;
    ce.code = halcyon::ErrorCode::Connection;
    ce.message = "dead";
    driver.executeErrors.push_back(ce);  // first row's execute fails

    auto batch = halcyon::batchOf<Pair>({{1, "a"}, {2, "b"}});
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, halcyon::ErrorCode::Connection);
    // The dead connection is discarded (markBroken), not returned to the pool.
    EXPECT_EQ(driver.disconnectCalls, 1);
}

TEST(Batch, TupleRowsBuildPositionalBinds) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    driver.execRowCounts.push_back(1);
    auto db = Database::open(driver, "X", noThread()).value();

    // No HALCYON_REFLECT needed — explicit tuple rows.
    auto batch = halcyon::batchOf({
        std::make_tuple(std::int64_t{1}, std::string{"a"}),
        std::make_tuple(std::int64_t{2}, std::string{"b"}),
    });
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 2);
    EXPECT_EQ(driver.preparedSql.size(), 1u);  // single prepare reused
}
