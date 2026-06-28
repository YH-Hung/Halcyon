#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "halcyon/connection.hpp"
#include "mock_cli_driver.hpp"

using halcyon::ResultSet;
using halcyon::detail::cli::Null;
using halcyon::detail::cli::Value;
using halcyon::testing::MockCliDriver;

namespace {
MockCliDriver::ScriptedRows usersGrid() {
    return MockCliDriver::ScriptedRows{
        {"id", "name"},
        {{Value{std::int64_t{1}}, Value{std::string{"ada"}}},
         {Value{std::int64_t{2}}, Value{std::string{"bob"}}}}};
}
}  // namespace

TEST(ResultSetTest, RangeIterationYieldsTuples) {
    MockCliDriver driver;
    auto conn = driver.connect({"x"}).value();
    driver.resultSets.push_back(usersGrid());
    auto stmt = driver.prepare(conn, "SELECT id, name FROM u").value();
    ASSERT_TRUE(driver.execute(stmt).ok());

    auto rs = ResultSet::create_borrowing(driver, stmt).value();
    EXPECT_EQ(rs.column_count(), 2u);

    std::vector<std::pair<int, std::string>> got;
    for (auto& row : rs) {
        auto [id, name] = row.as<int, std::string>();
        got.emplace_back(id, name);
    }
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].first, 1);
    EXPECT_EQ(got[0].second, "ada");
    EXPECT_EQ(got[1].second, "bob");
}

TEST(ResultSetTest, TryAsReturnsErrorOnArityMismatch) {
    MockCliDriver driver;
    auto conn = driver.connect({"x"}).value();
    driver.resultSets.push_back(usersGrid());
    auto stmt = driver.prepare(conn, "SELECT id, name FROM u").value();
    ASSERT_TRUE(driver.execute(stmt).ok());
    auto rs = ResultSet::create_borrowing(driver, stmt).value();

    auto it = rs.begin();
    ASSERT_NE(it, rs.end());
    auto bad = it->try_as<int>();  // 1 type vs 2 columns
    ASSERT_FALSE(bad.ok());
    EXPECT_EQ(bad.error().code, halcyon::ErrorCode::Mapping);
}

TEST(ResultSetTest, MidStreamFetchErrorIsSurfacedNotSilentlyDropped) {
    MockCliDriver driver;
    auto conn = driver.connect({"x"}).value();
    driver.resultSets.push_back(usersGrid());  // 2 rows available
    driver.fetchBlockSize = 1;                 // one row per block
    halcyon::Error e;
    e.code = halcyon::ErrorCode::Connection;
    e.message = "boom";
    e.retriable = true;
    driver.fetchBlockError = e;
    driver.failFetchBlockOnCall = 2;  // block 1 (row 0) ok; block 2 errors
    auto stmt = driver.prepare(conn, "SELECT id, name FROM u").value();
    ASSERT_TRUE(driver.execute(stmt).ok());

    auto rs = ResultSet::create_borrowing(driver, stmt).value();
    int rows = 0;
    for (auto& row : rs) {
        (void)row;
        ++rows;
    }
    EXPECT_EQ(rows, 1);
    ASSERT_FALSE(rs.ok());
    ASSERT_TRUE(rs.error().has_value());
    EXPECT_EQ(rs.error()->code, halcyon::ErrorCode::Connection);
    EXPECT_EQ(rs.error()->message, "boom");
}

TEST(ResultSetTest, RowsSpanMultipleBlocks) {
    MockCliDriver driver;
    auto conn = driver.connect({"x"}).value();
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"},
        {{Value{std::int64_t{1}}}, {Value{std::int64_t{2}}}, {Value{std::int64_t{3}}}, {Value{std::int64_t{4}}}}});
    driver.fetchBlockSize = 2;  // forces >= 2 blocks for 4 rows
    auto stmt = driver.prepare(conn, "SELECT id FROM u").value();
    ASSERT_TRUE(driver.execute(stmt).ok());
    auto rs = ResultSet::create_borrowing(driver, stmt).value();

    std::vector<int> got;
    for (auto& row : rs) got.push_back(std::get<0>(row.as<int>()));
    EXPECT_EQ(got, (std::vector<int>{1, 2, 3, 4}));
    EXPECT_TRUE(rs.ok());
}

TEST(ResultSetTest, CleanIterationLeavesNoError) {
    MockCliDriver driver;
    auto conn = driver.connect({"x"}).value();
    driver.resultSets.push_back(usersGrid());
    auto stmt = driver.prepare(conn, "SELECT id, name FROM u").value();
    ASSERT_TRUE(driver.execute(stmt).ok());
    auto rs = ResultSet::create_borrowing(driver, stmt).value();
    for (auto& row : rs) (void)row;
    EXPECT_TRUE(rs.ok());
    EXPECT_FALSE(rs.error().has_value());
}

TEST(ResultSetTest, AsThrowsMappingExceptionOnNullIntoNonOptional) {
    MockCliDriver driver;
    auto conn = driver.connect({"x"}).value();
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"name"}, {{Value{Null{}}}}});
    auto stmt = driver.prepare(conn, "SELECT name FROM u").value();
    ASSERT_TRUE(driver.execute(stmt).ok());
    auto rs = ResultSet::create_borrowing(driver, stmt).value();

    auto it = rs.begin();
    ASSERT_NE(it, rs.end());
    EXPECT_THROW(it->as<std::string>(), halcyon::MappingException);
}
