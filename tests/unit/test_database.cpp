#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "halcyon/database.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;

namespace {
MockCliDriver::ScriptedRows idName(std::int64_t id, std::string name) {
    return MockCliDriver::ScriptedRows{
        {"id", "name"},
        {{halcyon::detail::cli::Value{id},
          halcyon::detail::cli::Value{std::move(name)}}}};
}
PoolConfig noThread() {
    PoolConfig c;
    c.startMaintenanceThread = false;
    return c;
}
}  // namespace

struct Person {
    std::int64_t id;
    std::string name;
};
HALCYON_REFLECT(Person, id, name);

TEST(Database, OpenWarmsPoolToMin) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 2;
    auto db = Database::open(driver, "DATABASE=X;", cfg);
    ASSERT_TRUE(db.ok()) << db.error().message;
    EXPECT_EQ(driver.connectCalls, 2);
}

TEST(Database, ExecuteReturnsRowCount) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(3);
    auto db = Database::open(driver, "X", noThread()).value();
    auto n = db.execute("UPDATE t SET a=? WHERE id=?", 1, 5);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 3);
}

TEST(Database, QueryIteratesRows) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"},
        {{halcyon::detail::cli::Value{std::int64_t{1}}},
         {halcyon::detail::cli::Value{std::int64_t{2}}},
         {halcyon::detail::cli::Value{std::int64_t{4}}}}});
    auto db = Database::open(driver, "X", noThread()).value();
    auto qr = db.query("SELECT id FROM t WHERE id > ?", 0);
    ASSERT_TRUE(qr.ok());
    std::int64_t sum = 0;
    for (auto& row : qr.value()) sum += std::get<0>(row.as<std::int64_t>());
    EXPECT_EQ(sum, 7);
}

TEST(Database, QueryReleasesLeaseWhenResultDropped) {
    MockCliDriver driver;
    driver.resultSets.push_back(idName(1, "a"));
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    auto db = Database::open(driver, "X", cfg).value();
    { auto qr = db.query("SELECT id, name FROM t"); ASSERT_TRUE(qr.ok()); }
    // lease returned → a second single-slot query still works
    driver.resultSets.push_back(idName(2, "b"));
    auto qr2 = db.query("SELECT id, name FROM t");
    ASSERT_TRUE(qr2.ok());
}

TEST(Database, QueryAsMapsStructs) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id", "name"},
        {{halcyon::detail::cli::Value{std::int64_t{1}},
          halcyon::detail::cli::Value{std::string{"ann"}}},
         {halcyon::detail::cli::Value{std::int64_t{2}},
          halcyon::detail::cli::Value{std::string{"bob"}}}}});
    auto db = Database::open(driver, "X", noThread()).value();
    auto people = db.queryAs<Person>("SELECT id, name FROM people");
    ASSERT_TRUE(people.ok());
    ASSERT_EQ(people.value().size(), 2u);
    EXPECT_EQ(people.value()[1].name, "bob");
}

TEST(Database, NamedParamsThroughFacade) {
    MockCliDriver driver;
    driver.resultSets.push_back(idName(7, "z"));
    auto db = Database::open(driver, "X", noThread()).value();
    auto qr = db.query("SELECT id, name FROM t WHERE id = :id",
                       halcyon::params{{"id", 7}});
    ASSERT_TRUE(qr.ok());
    int rows = 0;
    for (auto& row : qr.value()) { (void)row; ++rows; }
    EXPECT_EQ(rows, 1);
}

TEST(Database, ThrowingOverloadUnwraps) {
    MockCliDriver driver;
    driver.executeErrors.push_back([] {
        halcyon::Error e;
        e.code = halcyon::ErrorCode::Syntax;
        e.message = "boom";
        return e;
    }());
    auto db = Database::open(driver, "X", noThread()).value();
    EXPECT_THROW(db.executeOrThrow("BAD SQL"), halcyon::Exception);
}
