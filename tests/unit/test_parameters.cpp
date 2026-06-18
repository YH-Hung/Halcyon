#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "halcyon/parameters.hpp"

using halcyon::params;
using halcyon::ErrorCode;
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
