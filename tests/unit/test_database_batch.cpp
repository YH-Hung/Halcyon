#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "halcyon/database.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;
using Value = halcyon::detail::cli::Value;
using halcyon::detail::cli::Null;

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

TEST(Batch, DelegatesToDriverReturningTotal) {
    MockCliDriver driver;
    driver.batchRowCounts.push_back(3);
    auto db = Database::open(driver, "X", noThread()).value();

    auto batch = halcyon::batchOf<Pair>({{1, "a"}, {2, "b"}, {3, "c"}});
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 3);
    EXPECT_EQ(driver.executeBatchCalls, 1);      // one array call, not per row
    EXPECT_EQ(driver.preparedSql.size(), 1u);    // single prepare
    ASSERT_EQ(driver.lastBatchRows.size(), 3u);  // all rows handed to the driver
    EXPECT_EQ(driver.lastBatchRows[0].size(), 2u);
}

TEST(Batch, EmptyBatchSkipsDriver) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", halcyon::Batch{});
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 0);
    EXPECT_EQ(driver.executeBatchCalls, 0);
    EXPECT_EQ(driver.preparedSql.size(), 0u);
}

TEST(Batch, DriverErrorPropagates) {
    MockCliDriver driver;
    halcyon::Error e;
    e.code = halcyon::ErrorCode::Constraint;
    e.message = "dup";
    driver.batchErrors.push_back(e);
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
    driver.batchErrors.push_back(ce);

    auto batch = halcyon::batchOf<Pair>({{1, "a"}, {2, "b"}});
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, halcyon::ErrorCode::Connection);
    EXPECT_EQ(driver.disconnectCalls, 1);  // broken connection discarded
}

TEST(Batch, RaggedRowsRejectedBeforeDriver) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();

    halcyon::Batch b;
    b.rows = {{Value{std::int64_t{1}}, Value{std::string{"a"}}},
              {Value{std::int64_t{2}}}};  // 2 cols then 1 col
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", b);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, halcyon::ErrorCode::Mapping);
    EXPECT_EQ(driver.executeBatchCalls, 0);
    EXPECT_EQ(driver.preparedSql.size(), 0u);
}

TEST(Batch, MixedTypeColumnRejectedBeforeDriver) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();

    halcyon::Batch b;
    b.rows = {{Value{std::int64_t{1}}},
              {Value{std::string{"x"}}}};  // column 0: int64 then string
    auto n = db.executeBatch("INSERT INTO t(a) VALUES (?)", b);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, halcyon::ErrorCode::Mapping);
    EXPECT_EQ(driver.executeBatchCalls, 0);
}

TEST(Batch, NullsInColumnPassThrough) {
    MockCliDriver driver;
    driver.batchRowCounts.push_back(2);
    auto db = Database::open(driver, "X", noThread()).value();

    halcyon::Batch b;
    b.rows = {{Value{std::int64_t{1}}, Value{Null{}}},
              {Value{std::int64_t{2}}, Value{std::string{"b"}}}};
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", b);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 2);
    ASSERT_EQ(driver.lastBatchRows.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<Null>(driver.lastBatchRows[0][1]));
}

TEST(Batch, AllNullColumnIsValid) {
    MockCliDriver driver;
    driver.batchRowCounts.push_back(2);
    auto db = Database::open(driver, "X", noThread()).value();

    halcyon::Batch b;
    b.rows = {{Value{std::int64_t{1}}, Value{Null{}}},
              {Value{std::int64_t{2}}, Value{Null{}}}};  // column 1 all-NULL
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", b);
    ASSERT_TRUE(n.ok());  // all-NULL column carries no type conflict
    EXPECT_EQ(n.value(), 2);
    EXPECT_EQ(driver.executeBatchCalls, 1);
}

TEST(Batch, TupleRowsBuildPositionalBinds) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();

    auto batch = halcyon::batchOf({
        std::make_tuple(std::int64_t{1}, std::string{"a"}),
        std::make_tuple(std::int64_t{2}, std::string{"b"}),
    });
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 2);  // mock default return == rows.size()
    EXPECT_EQ(driver.preparedSql.size(), 1u);
    EXPECT_EQ(driver.executeBatchCalls, 1);
}

TEST(Batch, TupleVectorRowsBuildPositionalBinds) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();

    // Rows assembled in a vector (not an initializer_list) of tuples — resolves
    // to the vector<tuple> overload, not the reflected-struct one.
    std::vector<std::tuple<std::int64_t, std::string>> rows;
    rows.emplace_back(1, "a");
    rows.emplace_back(2, "b");
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)",
                             halcyon::batchOf(rows));
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 2);  // mock default return == rows.size()
    ASSERT_EQ(driver.lastBatchRows.size(), 2u);
    EXPECT_EQ(driver.lastBatchRows[0].size(), 2u);
    EXPECT_EQ(driver.executeBatchCalls, 1);
}

TEST(Batch, ExecutesInsideTransaction) {
    MockCliDriver driver;
    driver.batchRowCounts.push_back(2);
    auto db = Database::open(driver, "X", noThread()).value();

    auto tx = db.begin().value();
    auto batch = halcyon::batchOf<Pair>({{1, "a"}, {2, "b"}});
    auto n = tx.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 2);
    ASSERT_TRUE(tx.commit().ok());
    EXPECT_EQ(driver.executeBatchCalls, 1);
    EXPECT_EQ(driver.commitCalls, 1);
    // autocommit toggled OFF for the unit of work, then back ON on commit.
    ASSERT_GE(driver.autoCommitCalls.size(), 2u);
    EXPECT_FALSE(driver.autoCommitCalls.front());
    EXPECT_TRUE(driver.autoCommitCalls.back());
}
