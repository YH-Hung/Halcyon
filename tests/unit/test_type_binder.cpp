#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "halcyon/types.hpp"

using halcyon::ErrorCode;
using halcyon::TypeBinder;
using halcyon::detail::cli::Null;
using halcyon::detail::cli::Value;

TEST(TypeBinder, IntegerRoundTripAndNarrowing) {
    EXPECT_EQ(TypeBinder<std::int64_t>::to_value(std::int64_t{5}),
              Value{std::int64_t{5}});
    EXPECT_EQ(TypeBinder<std::int32_t>::from_value(Value{std::int64_t{42}}).value(),
              42);
    // Out-of-range narrowing is a Mapping error, never silent truncation.
    auto bad = TypeBinder<std::int32_t>::from_value(
        Value{std::int64_t{1} << 40});
    ASSERT_FALSE(bad.ok());
    EXPECT_EQ(bad.error().code, ErrorCode::Mapping);
}

TEST(TypeBinder, BoolReadsFromBoolOrInt) {
    EXPECT_TRUE(TypeBinder<bool>::from_value(Value{true}).value());
    EXPECT_TRUE(TypeBinder<bool>::from_value(Value{std::int64_t{1}}).value());
    EXPECT_FALSE(TypeBinder<bool>::from_value(Value{std::int64_t{0}}).value());
}

TEST(TypeBinder, DoubleAndString) {
    EXPECT_EQ(TypeBinder<double>::from_value(Value{2.5}).value(), 2.5);
    EXPECT_EQ(TypeBinder<std::string>::from_value(Value{std::string{"hi"}}).value(),
              "hi");
}

TEST(TypeBinder, NullIntoNonOptionalIsMappingError) {
    auto r = TypeBinder<std::int64_t>::from_value(Value{Null{}});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Mapping);
}

TEST(TypeBinder, OptionalMapsNullToNullopt) {
    auto none = TypeBinder<std::optional<int>>::from_value(Value{Null{}});
    ASSERT_TRUE(none.ok());
    EXPECT_FALSE(none.value().has_value());

    auto some = TypeBinder<std::optional<int>>::from_value(Value{std::int64_t{9}});
    ASSERT_TRUE(some.ok());
    ASSERT_TRUE(some.value().has_value());
    EXPECT_EQ(*some.value(), 9);

    EXPECT_TRUE(std::holds_alternative<Null>(
        TypeBinder<std::optional<int>>::to_value(std::nullopt)));
}

TEST(TypeBinder, TypeMismatchIsMappingError) {
    auto r = TypeBinder<std::int64_t>::from_value(Value{std::string{"x"}});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Mapping);
}

TEST(TypeBinder, BindOnlyStringViewAndCharPtr) {
    EXPECT_EQ(halcyon::detail::to_value(std::string_view{"abc"}),
              Value{std::string{"abc"}});
    EXPECT_EQ(halcyon::detail::to_value("lit"), Value{std::string{"lit"}});
}

TEST(TypeBinder, Traits) {
    static_assert(halcyon::is_bindable<int>::value);
    static_assert(halcyon::is_readable<int>::value);
    static_assert(halcyon::is_bindable<std::optional<std::string>>::value);
}
