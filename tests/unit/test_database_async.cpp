#include <gtest/gtest.h>

#include <cstdint>
#include <future>
#include <string>
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

struct Num {
    std::int64_t n;
};
HALCYON_REFLECT(Num, n);

TEST(DatabaseAsync, ExecuteAsyncReturnsCount) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(4);
    auto db = Database::open(driver, "X", noThread()).value();
    auto f = db.executeAsync("UPDATE t SET a=? WHERE id=?", 1, 2);
    auto r = f.get();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 4);
}

TEST(DatabaseAsync, QueryAsyncMaterializesRows) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"n"},
        {{halcyon::detail::cli::Value{std::int64_t{2}}},
         {halcyon::detail::cli::Value{std::int64_t{5}}}}});
    auto db = Database::open(driver, "X", noThread()).value();
    auto f = db.queryAsync<Num>("SELECT n FROM t");
    auto r = f.get();
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().size(), 2u);
    EXPECT_EQ(r.value()[1].n, 5);
}
