#include <gtest/gtest.h>

#include "capturing_logger.hpp"
#include "halcyon/connection.hpp"
#include "halcyon/transaction.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Connection;
using halcyon::Transaction;
using halcyon::testing::MockCliDriver;

namespace {
Connection open(MockCliDriver& d) {
    return Connection::open(d, {"x"}).value();
}
}  // namespace

TEST(SeamTransaction, MockRecordsAutoCommitCommitRollback) {
    MockCliDriver driver;
    auto conn = open(driver);
    auto h = conn.handle();
    ASSERT_TRUE(driver.setAutoCommit(h, false).ok());
    ASSERT_TRUE(driver.commit(h).ok());
    ASSERT_TRUE(driver.rollback(h).ok());
    EXPECT_EQ(driver.autoCommitCalls.size(), 1u);
    EXPECT_FALSE(driver.autoCommitCalls.front());
    EXPECT_EQ(driver.commitCalls, 1);
    EXPECT_EQ(driver.rollbackCalls, 1);
}

#include <cstdint>

namespace {
MockCliDriver::ScriptedRows oneInt(std::int64_t v) {
    return MockCliDriver::ScriptedRows{
        {"c"}, {{halcyon::detail::cli::Value{v}}}};
}
}  // namespace

TEST(Transaction, BeginDisablesAutoCommitCommitRestores) {
    MockCliDriver driver;
    auto conn = open(driver);
    {
        auto tx = conn.begin();
        ASSERT_TRUE(tx.ok());
        ASSERT_TRUE(tx.value().active());
        auto n = tx.value().execute("INSERT INTO t VALUES (?)", 1);
        ASSERT_TRUE(n.ok());
        ASSERT_TRUE(tx.value().commit().ok());
        EXPECT_FALSE(tx.value().active());
    }
    ASSERT_EQ(driver.autoCommitCalls.size(), 2u);
    EXPECT_FALSE(driver.autoCommitCalls[0]);  // begin → off
    EXPECT_TRUE(driver.autoCommitCalls[1]);   // commit → on
    EXPECT_EQ(driver.commitCalls, 1);
    EXPECT_EQ(driver.rollbackCalls, 0);
}

TEST(Transaction, AutoRollsBackWhenNotCommitted) {
    MockCliDriver driver;
    auto conn = open(driver);
    {
        auto tx = conn.begin().value();
        ASSERT_TRUE(tx.execute("UPDATE t SET a=?", 2).ok());
        // no commit() → dtor rolls back
    }
    EXPECT_EQ(driver.commitCalls, 0);
    EXPECT_EQ(driver.rollbackCalls, 1);
    ASSERT_EQ(driver.autoCommitCalls.size(), 2u);
    EXPECT_TRUE(driver.autoCommitCalls[1]);  // rollback restores autocommit
}

TEST(Transaction, ExplicitRollback) {
    MockCliDriver driver;
    auto conn = open(driver);
    auto tx = conn.begin().value();
    ASSERT_TRUE(tx.rollback().ok());
    EXPECT_FALSE(tx.active());
    EXPECT_EQ(driver.rollbackCalls, 1);
}

TEST(Transaction, QueryInsideTransaction) {
    MockCliDriver driver;
    driver.resultSets.push_back(oneInt(7));
    auto conn = open(driver);
    auto tx = conn.begin().value();
    auto rs = tx.query("SELECT c FROM t");
    ASSERT_TRUE(rs.ok());
    std::int64_t sum = 0;
    for (auto& row : rs.value()) sum += std::get<0>(row.as<std::int64_t>());
    EXPECT_EQ(sum, 7);
    ASSERT_TRUE(tx.commit().ok());
}

TEST(Transaction, MoveDoesNotDoubleRollback) {
    MockCliDriver driver;
    auto conn = open(driver);
    {
        auto a = conn.begin().value();
        auto b = std::move(a);  // b now owns the unit of work
        ASSERT_TRUE(b.execute("DELETE FROM t").ok());
    }
    EXPECT_EQ(driver.rollbackCalls, 1);  // exactly once, from b
}

TEST(TransactionLogging, PoisonedCommitIsLogged) {
    halcyon::testing::MockCliDriver drv;
    halcyon::testing::CapturingLogger logger;
    auto cr = halcyon::Connection::open(drv, {"dsn"}, 0, nullptr, &logger);
    ASSERT_TRUE(cr.ok());
    halcyon::Connection conn = std::move(cr.value());
    auto txr = conn.begin();
    ASSERT_TRUE(txr.ok());
    halcyon::Error dead;
    dead.code = halcyon::ErrorCode::Connection;
    drv.txnErrors.push_back(dead);  // commit() fails
    auto c = txr.value().commit();
    EXPECT_FALSE(c.ok());
    EXPECT_TRUE(txr.value().poisoned());
    EXPECT_EQ(logger.count("txn.poisoned"), 1u);
}
