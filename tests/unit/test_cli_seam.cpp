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

TEST(CliSeamStatement, ScriptedResultSetIsBlockFetchable) {
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

    auto blk = driver.fetchBlock(st, 100);
    ASSERT_TRUE(blk.ok());
    ASSERT_EQ(blk.value().size(), 2u);
    EXPECT_EQ(blk.value()[0][0], Value{std::int64_t{1}});
    EXPECT_EQ(blk.value()[0][1], Value{std::string{"ada"}});
    EXPECT_EQ(blk.value()[1][1], Value{Null{}});
    EXPECT_TRUE(driver.fetchBlock(st, 100).value().empty());  // end
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

TEST(CliSeamStatement, CloseCursorIsCallableAndIdempotent) {
    MockCliDriver driver;
    auto conn = driver.connect(ConnectionParams{"x"}).value();
    auto st = driver.prepare(conn, "SELECT id FROM t").value();

    auto a = driver.closeCursor(st);
    auto b = driver.closeCursor(st);  // idempotent: no open cursor

    EXPECT_TRUE(a.ok());
    EXPECT_TRUE(b.ok());
    EXPECT_EQ(driver.closeCursorCalls, 2);
}

TEST(CliSeamBlock, FetchBlockReturnsAllRowsThenEmpty) {
    MockCliDriver driver;
    auto conn = driver.connect(ConnectionParams{"x"}).value();
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id", "name"},
        {{Value{std::int64_t{1}}, Value{std::string{"ada"}}},
         {Value{std::int64_t{2}}, Value{Null{}}}}});
    auto st = driver.prepare(conn, "SELECT id, name FROM t").value();
    ASSERT_TRUE(driver.execute(st).ok());

    auto blk = driver.fetchBlock(st, 100);
    ASSERT_TRUE(blk.ok());
    ASSERT_EQ(blk.value().size(), 2u);
    EXPECT_EQ(blk.value()[0][0], Value{std::int64_t{1}});
    EXPECT_EQ(blk.value()[1][1], Value{Null{}});

    auto end = driver.fetchBlock(st, 100);
    ASSERT_TRUE(end.ok());
    EXPECT_TRUE(end.value().empty());  // exhausted -> empty
}

TEST(CliSeamBlock, FetchBlockHonorsMaxRowsAndResumes) {
    MockCliDriver driver;
    auto conn = driver.connect(ConnectionParams{"x"}).value();
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"},
        {{Value{std::int64_t{1}}}, {Value{std::int64_t{2}}}, {Value{std::int64_t{3}}}}});
    auto st = driver.prepare(conn, "SELECT id FROM t").value();
    ASSERT_TRUE(driver.execute(st).ok());

    auto a = driver.fetchBlock(st, 2);
    ASSERT_TRUE(a.ok());
    ASSERT_EQ(a.value().size(), 2u);  // capped at maxRows
    auto b = driver.fetchBlock(st, 2);
    ASSERT_TRUE(b.ok());
    ASSERT_EQ(b.value().size(), 1u);  // remainder
    EXPECT_EQ(b.value()[0][0], Value{std::int64_t{3}});
}

TEST(CliSeamBlock, FetchBlockPropagatesScriptedError) {
    MockCliDriver driver;
    auto conn = driver.connect(ConnectionParams{"x"}).value();
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"}, {{Value{std::int64_t{1}}}}});
    Error e;
    e.code = ErrorCode::Connection;
    e.message = "drop";
    driver.fetchBlockError = e;
    driver.failFetchBlockOnCall = 1;
    auto st = driver.prepare(conn, "SELECT id FROM t").value();
    ASSERT_TRUE(driver.execute(st).ok());

    auto blk = driver.fetchBlock(st, 100);
    ASSERT_FALSE(blk.ok());
    EXPECT_EQ(blk.error().code, ErrorCode::Connection);
}
