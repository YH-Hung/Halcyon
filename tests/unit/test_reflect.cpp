#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "halcyon/connection.hpp"
#include "halcyon/types.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Connection;
using halcyon::detail::cli::Null;
using halcyon::detail::cli::Value;
using halcyon::testing::MockCliDriver;

struct User {
    int id;
    std::string name;
    std::optional<int> age;
};
HALCYON_REFLECT(User, id, name, age);

TEST(Reflect, FieldCountAndFlag) {
    static_assert(halcyon::reflect::Reflected<User>::value);
    EXPECT_EQ(halcyon::reflect::Reflected<User>::field_count, 3u);
}

TEST(Reflect, QueryAsMapsRowsToStructs) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id", "name", "age"},
        {{Value{std::int64_t{1}}, Value{std::string{"ada"}}, Value{std::int64_t{36}}},
         {Value{std::int64_t{2}}, Value{std::string{"bob"}}, Value{Null{}}}}});
    auto c = Connection::open(driver, {"x"}).value();

    auto users = c.queryAs<User>("SELECT id, name, age FROM users");
    ASSERT_TRUE(users.ok());
    ASSERT_EQ(users.value().size(), 2u);
    EXPECT_EQ(users.value()[0].id, 1);
    EXPECT_EQ(users.value()[0].name, "ada");
    ASSERT_TRUE(users.value()[0].age.has_value());
    EXPECT_EQ(*users.value()[0].age, 36);
    EXPECT_FALSE(users.value()[1].age.has_value());  // NULL -> nullopt
}

TEST(Reflect, QueryAsArityMismatchIsMappingError) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id", "name"},
        {{Value{std::int64_t{1}}, Value{std::string{"ada"}}}}});  // 2 cols, 3 fields
    auto c = Connection::open(driver, {"x"}).value();

    auto users = c.queryAs<User>("SELECT id, name FROM users");
    ASSERT_FALSE(users.ok());
    EXPECT_EQ(users.error().code, halcyon::ErrorCode::Mapping);
}
