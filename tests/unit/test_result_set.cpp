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
