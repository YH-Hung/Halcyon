#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "halcyon/parameters.hpp"

using halcyon::ErrorCode;
using halcyon::params;
using halcyon::detail::cli::Value;

TEST(Parameters, PacksAnonymousArgsInOrder) {
    auto v = halcyon::detail::pack_params(21, std::string{"NYC"}, true);
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], Value{std::int64_t{21}});
    EXPECT_EQ(v[1], Value{std::string{"NYC"}});
    EXPECT_EQ(v[2], Value{true});
}

TEST(Parameters, PacksStringLiteral) {
    auto v = halcyon::detail::pack_params("lit");
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], Value{std::string{"lit"}});
}

TEST(Parameters, NamedRewriteProducesPositionalSqlAndValues) {
    auto r = halcyon::detail::bind_named(
        "SELECT id FROM u WHERE age > :age AND city = :city",
        params{{"age", 21}, {"city", std::string{"NYC"}}});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().sql, "SELECT id FROM u WHERE age > ? AND city = ?");
    ASSERT_EQ(r.value().params.size(), 2u);
    EXPECT_EQ(r.value().params[0], Value{std::int64_t{21}});
    EXPECT_EQ(r.value().params[1], Value{std::string{"NYC"}});
}

TEST(Parameters, NamedSupportsRepeatedPlaceholder) {
    auto r = halcyon::detail::bind_named("VALUES (:x, :x)", params{{"x", 5}});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().sql, "VALUES (?, ?)");
    ASSERT_EQ(r.value().params.size(), 2u);
    EXPECT_EQ(r.value().params[1], Value{std::int64_t{5}});
}

TEST(Parameters, UnknownNamedParamIsMappingError) {
    auto r = halcyon::detail::bind_named("WHERE a = :missing", params{{"x", 1}});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Mapping);
}

TEST(Parameters, DoubleColonIsLeftLiteral) {
    auto r = halcyon::detail::bind_named("SELECT a::int FROM t", params{});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().sql, "SELECT a::int FROM t");
    EXPECT_TRUE(r.value().params.empty());
}

TEST(Parameters, NamedSkipsSingleQuotedStringLiteral) {
    auto r = halcyon::detail::bind_named(
        "SELECT ':id' AS lit FROM t WHERE id = :id", params{{"id", 7}});
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value().sql, "SELECT ':id' AS lit FROM t WHERE id = ?");
    ASSERT_EQ(r.value().params.size(), 1u);
    EXPECT_EQ(r.value().params[0], Value{std::int64_t{7}});
}

TEST(Parameters, NamedHandlesDoubledQuoteEscapeInString) {
    auto r = halcyon::detail::bind_named("SELECT 'it''s :x' FROM t WHERE a = :a",
                                         params{{"a", 4}});
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value().sql, "SELECT 'it''s :x' FROM t WHERE a = ?");
    ASSERT_EQ(r.value().params.size(), 1u);
    EXPECT_EQ(r.value().params[0], Value{std::int64_t{4}});
}

TEST(Parameters, NamedSkipsDelimitedIdentifier) {
    auto r = halcyon::detail::bind_named("SELECT \":id\" FROM t WHERE z = :z",
                                         params{{"z", 3}});
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value().sql, "SELECT \":id\" FROM t WHERE z = ?");
    ASSERT_EQ(r.value().params.size(), 1u);
}

TEST(Parameters, NamedSkipsLineComment) {
    auto r = halcyon::detail::bind_named(
        "SELECT 1 -- nope :id here\nFROM t WHERE x = :x", params{{"x", 1}});
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value().sql, "SELECT 1 -- nope :id here\nFROM t WHERE x = ?");
    ASSERT_EQ(r.value().params.size(), 1u);
}

TEST(Parameters, NamedSkipsBlockComment) {
    auto r = halcyon::detail::bind_named(
        "SELECT /* :id and :nope */ x FROM t WHERE y = :y", params{{"y", 2}});
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value().sql, "SELECT /* :id and :nope */ x FROM t WHERE y = ?");
    ASSERT_EQ(r.value().params.size(), 1u);
}
