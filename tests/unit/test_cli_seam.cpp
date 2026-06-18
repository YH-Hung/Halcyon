#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Error;
using halcyon::ErrorCode;
using halcyon::detail::cli::ConnectionHandle;
using halcyon::detail::cli::ConnectionParams;
using halcyon::detail::cli::Null;
using halcyon::detail::cli::StatementHandle;
using halcyon::detail::cli::Value;
using halcyon::testing::MockCliDriver;

TEST(CliSeam, ConnectReturnsValidHandle) {
    MockCliDriver driver;
    auto r = driver.connect(ConnectionParams{"DATABASE=SAMPLE;"});
    ASSERT_TRUE(r.ok());
    EXPECT_NE(r.value(), ConnectionHandle::invalid);
    EXPECT_EQ(driver.connectCalls, 1);
    ASSERT_EQ(driver.connectParams.size(), 1u);
    EXPECT_EQ(driver.connectParams[0].connectionString, "DATABASE=SAMPLE;");
}

TEST(CliSeam, ConnectPropagatesScriptedError) {
    MockCliDriver driver;
    Error e;
    e.code = ErrorCode::Connection;
    e.sqlstate = "08001";
    e.retriable = true;
    driver.connectErrors.push_back(e);

    auto r = driver.connect(ConnectionParams{"bad"});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Connection);
}

TEST(CliSeam, IsAliveHonorsScriptedResults) {
    MockCliDriver driver;
    driver.aliveResults = {false, true};
    auto h = driver.connect(ConnectionParams{"x"}).value();
    EXPECT_FALSE(driver.isAlive(h).value());
    EXPECT_TRUE(driver.isAlive(h).value());
    EXPECT_EQ(driver.aliveCalls, 2);
}

TEST(CliSeam, DisconnectCounts) {
    MockCliDriver driver;
    auto h = driver.connect(ConnectionParams{"x"}).value();
    ASSERT_TRUE(driver.disconnect(h).ok());
    EXPECT_EQ(driver.disconnectCalls, 1);
}

TEST(CliSeamStatement, PrepareBindExecuteReturnsRowsAffected) {
    MockCliDriver driver;
    auto conn = driver.connect(ConnectionParams{"x"}).value();
    driver.execRowCounts.push_back(3);

    auto st = driver.prepare(conn, "UPDATE t SET a = ? WHERE b = ?");
    ASSERT_TRUE(st.ok());
    ASSERT_TRUE(driver.bindParams(st.value(),
                                  {Value{std::int64_t{7}}, Value{std::string{"k"}}})
                    .ok());
    auto rows = driver.execute(st.value());
    ASSERT_TRUE(rows.ok());
    EXPECT_EQ(rows.value(), 3);

    ASSERT_EQ(driver.statements.at(st.value()).boundParams.size(), 2u);
    EXPECT_EQ(driver.statements.at(st.value()).boundParams[0], Value{std::int64_t{7}});
    EXPECT_EQ(driver.preparedSql.back(), "UPDATE t SET a = ? WHERE b = ?");
    ASSERT_TRUE(driver.finalize(st.value()).ok());
}

TEST(CliSeamStatement, ScriptedResultSetIsFetchable) {
    MockCliDriver driver;
    auto conn = driver.connect(ConnectionParams{"x"}).value();
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id", "name"},
        {{Value{std::int64_t{1}}, Value{std::string{"ada"}}},
         {Value{std::int64_t{2}}, Value{Null{}}}}});

    auto st = driver.prepare(conn, "SELECT id, name FROM t").value();
    ASSERT_TRUE(driver.execute(st).ok());
    EXPECT_EQ(driver.columnCount(st).value(), 2u);
    EXPECT_EQ(driver.columnName(st, 1).value(), "name");

    ASSERT_TRUE(driver.fetch(st).value());
    EXPECT_EQ(driver.getColumn(st, 0).value(), Value{std::int64_t{1}});
    EXPECT_EQ(driver.getColumn(st, 1).value(), Value{std::string{"ada"}});
    ASSERT_TRUE(driver.fetch(st).value());
    EXPECT_EQ(driver.getColumn(st, 1).value(), Value{Null{}});
    EXPECT_FALSE(driver.fetch(st).value());  // end of cursor
}

TEST(CliSeamStatement, PrepareErrorIsScriptable) {
    MockCliDriver driver;
    auto conn = driver.connect(ConnectionParams{"x"}).value();
    Error e;
    e.code = ErrorCode::Syntax;
    e.sqlstate = "42601";
    driver.prepareErrors.push_back(e);
    auto st = driver.prepare(conn, "SELEKT 1");
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().code, ErrorCode::Syntax);
}
