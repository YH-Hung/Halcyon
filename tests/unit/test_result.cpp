#include <gtest/gtest.h>

#include <string>

#include "halcyon/result.hpp"

using halcyon::Error;
using halcyon::ErrorCode;
using halcyon::Result;

static Error makeError() {
    Error e;
    e.code = ErrorCode::Syntax;
    e.message = "bad sql";
    return e;
}

TEST(ResultT, HoldsValue) {
    Result<int> r(42);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r.value(), 42);
}

TEST(ResultT, HoldsError) {
    Result<int> r(makeError());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Syntax);
}

TEST(ResultT, ValueThrowsOnError) {
    Result<int> r(makeError());
    EXPECT_THROW(r.value(), halcyon::QueryException);
}

TEST(ResultT, ValueOrReturnsFallbackOnError) {
    Result<int> r(makeError());
    EXPECT_EQ(r.value_or(7), 7);
    Result<int> ok(3);
    EXPECT_EQ(ok.value_or(7), 3);
}

TEST(ResultT, MapTransformsValueAndPropagatesError) {
    Result<int> r(21);
    Result<int> doubled = r.map([](int v) { return v * 2; });
    ASSERT_TRUE(doubled.ok());
    EXPECT_EQ(doubled.value(), 42);

    Result<int> err(makeError());
    Result<int> mapped = err.map([](int v) { return v * 2; });
    ASSERT_FALSE(mapped.ok());
    EXPECT_EQ(mapped.error().code, ErrorCode::Syntax);
}

TEST(ResultT, AndThenChains) {
    Result<int> r(10);
    Result<std::string> s =
        r.and_then([](int v) { return Result<std::string>(std::to_string(v)); });
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s.value(), "10");
}

TEST(ResultVoid, OkAndError) {
    Result<void> ok;
    EXPECT_TRUE(ok.ok());
    Result<void> err(makeError());
    EXPECT_FALSE(err.ok());
    EXPECT_THROW(err.value(), halcyon::QueryException);
}
