#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "halcyon/connection.hpp"
#include "halcyon/parameters.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Connection;
using halcyon::params;
using halcyon::detail::cli::ConnectionParams;
using halcyon::detail::cli::Value;
using halcyon::testing::MockCliDriver;

TEST(ConnectionTest, OpenConnectsAndDisconnectsOnDestruction) {
    MockCliDriver driver;
    {
        auto c = Connection::open(driver, {"DATABASE=SAMPLE;"});
        ASSERT_TRUE(c.ok());
        EXPECT_EQ(driver.connectCalls, 1);
    }
    EXPECT_EQ(driver.disconnectCalls, 1);
}

TEST(ConnectionTest, ExecuteBindsAnonymousParamsAndReturnsRowCount) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(2);
    auto c = Connection::open(driver, {"x"}).value();

    auto n = c.execute("UPDATE t SET a = ? WHERE b = ?", 9, std::string{"k"});
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 2);
    ASSERT_FALSE(driver.preparedSql.empty());
    EXPECT_EQ(driver.preparedSql.back(), "UPDATE t SET a = ? WHERE b = ?");
    // the prepared statement's bound params were [9, "k"]
    auto& st = driver.statements.begin()->second;
    ASSERT_EQ(st.boundParams.size(), 2u);
    EXPECT_EQ(st.boundParams[0], Value{std::int64_t{9}});
}

TEST(ConnectionTest, QueryAnonymousReturnsIterableResultSet) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"}, {{Value{std::int64_t{10}}}, {Value{std::int64_t{20}}}}});
    auto c = Connection::open(driver, {"x"}).value();

    auto rs = c.query("SELECT id FROM t WHERE id > ?", 5);
    ASSERT_TRUE(rs.ok());
    long sum = 0;
    for (auto& row : rs.value()) sum += std::get<0>(row.as<int>());
    EXPECT_EQ(sum, 30);
}

TEST(ConnectionTest, QueryNamedRewritesPlaceholders) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{{"id"}, {}});
    auto c = Connection::open(driver, {"x"}).value();

    auto rs = c.query("SELECT id FROM t WHERE age > :age",
                      params{{"age", 21}});
    ASSERT_TRUE(rs.ok());
    EXPECT_EQ(driver.preparedSql.back(), "SELECT id FROM t WHERE age > ?");
    auto& st = driver.statements.begin()->second;
    ASSERT_EQ(st.boundParams.size(), 1u);
    EXPECT_EQ(st.boundParams[0], Value{std::int64_t{21}});
}

TEST(ConnectionTest, PreparedStatementIsReusable) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    driver.execRowCounts.push_back(1);
    auto c = Connection::open(driver, {"x"}).value();

    auto st = c.prepare("INSERT INTO t(a) VALUES (?)");
    ASSERT_TRUE(st.ok());
    EXPECT_EQ(st.value().execute_update(1).value(), 1);
    EXPECT_EQ(st.value().execute_update(2).value(), 1);
    EXPECT_EQ(driver.preparedSql.size(), 1u);  // prepared once, executed twice
}

TEST(ConnectionCache, RepeatedQueryReusesPreparedStatement) {
    MockCliDriver driver;
    // Two SELECTs of the same SQL; scripted empty result sets.
    driver.resultSets.push_back({{"id"}, {}});
    driver.resultSets.push_back({{"id"}, {}});
    auto conn = Connection::open(driver, ConnectionParams{"x"},
                                 /*statementCacheSize=*/8)
                    .value();

    { auto r = conn.query("SELECT id FROM t WHERE age > ?", 21); ASSERT_TRUE(r.ok()); }
    { auto r = conn.query("SELECT id FROM t WHERE age > ?", 21); ASSERT_TRUE(r.ok()); }

    EXPECT_EQ(driver.preparedSql.size(), 1u);  // prepared once, reused
}

TEST(ConnectionCache, DisabledByDefaultPreparesEveryCall) {
    MockCliDriver driver;
    driver.resultSets.push_back({{"id"}, {}});
    driver.resultSets.push_back({{"id"}, {}});
    auto conn = Connection::open(driver, ConnectionParams{"x"}).value();  // size 0

    { auto r = conn.query("SELECT id FROM t WHERE age > ?", 21); ASSERT_TRUE(r.ok()); }
    { auto r = conn.query("SELECT id FROM t WHERE age > ?", 21); ASSERT_TRUE(r.ok()); }

    EXPECT_EQ(driver.preparedSql.size(), 2u);  // no cache -> two prepares
}

TEST(ConnectionCache, OverlappingCursorsOnSameSqlOverflow) {
    MockCliDriver driver;
    driver.resultSets.push_back({{"id"}, {}});
    driver.resultSets.push_back({{"id"}, {}});
    auto conn = Connection::open(driver, ConnectionParams{"x"},
                                 /*statementCacheSize=*/8)
                    .value();

    auto r1 = conn.query("SELECT id FROM t", 0);  // held open (busy)
    ASSERT_TRUE(r1.ok());
    auto r2 = conn.query("SELECT id FROM t", 0);  // same sql, still busy
    ASSERT_TRUE(r2.ok());

    EXPECT_EQ(driver.preparedSql.size(), 2u);  // second is a transient overflow
}

TEST(ConnectionCache, ExecuteErrorDropsEntry) {
    MockCliDriver driver;
    driver.executeErrors.push_back([] {
        halcyon::Error e; e.code = halcyon::ErrorCode::Syntax; e.message = "boom";
        return e;
    }());
    auto conn = Connection::open(driver, ConnectionParams{"x"},
                                 /*statementCacheSize=*/8)
                    .value();

    auto bad = conn.execute("UPDATE t SET a = ? WHERE b = ?", 1, 2);
    ASSERT_FALSE(bad.ok());                // first execute fails -> poisoned
    auto ok = conn.execute("UPDATE t SET a = ? WHERE b = ?", 1, 2);
    ASSERT_TRUE(ok.ok());                  // re-prepared after drop

    EXPECT_EQ(driver.preparedSql.size(), 2u);
}
