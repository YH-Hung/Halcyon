#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "halcyon/parameters.hpp"
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

// --- spec §7 completion: Decimal + chrono date/time (string-carriage) ---

TEST(TypeBinder, DecimalRoundTripPreservesExactText) {
    halcyon::Decimal d{"123.4500"};
    EXPECT_EQ(TypeBinder<halcyon::Decimal>::to_value(d),
              Value{std::string{"123.4500"}});
    auto back = TypeBinder<halcyon::Decimal>::from_value(
        Value{std::string{"123.4500"}});
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value(), halcyon::Decimal{"123.4500"});  // exact scale kept
}

TEST(TypeBinder, DecimalNullAndTypeMismatchAreMappingErrors) {
    auto n = TypeBinder<halcyon::Decimal>::from_value(Value{Null{}});
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, ErrorCode::Mapping);
    auto m = TypeBinder<halcyon::Decimal>::from_value(Value{std::int64_t{5}});
    ASSERT_FALSE(m.ok());
    EXPECT_EQ(m.error().code, ErrorCode::Mapping);
}

TEST(TypeBinder, DateTimeTimestampRoundTrip) {
    EXPECT_EQ(TypeBinder<halcyon::Date>::to_value(halcyon::Date{"2026-06-21"}),
              Value{std::string{"2026-06-21"}});
    EXPECT_EQ(TypeBinder<halcyon::Time>::from_value(Value{std::string{"13:45:00"}})
                  .value(),
              halcyon::Time{"13:45:00"});
    auto ts = TypeBinder<halcyon::Timestamp>::from_value(
        Value{std::string{"2026-06-21 13:45:00.123456"}});
    ASSERT_TRUE(ts.ok());
    EXPECT_EQ(ts.value(), halcyon::Timestamp{"2026-06-21 13:45:00.123456"});
}

TEST(TypeBinder, TemporalNullIsMappingError) {
    EXPECT_FALSE(TypeBinder<halcyon::Timestamp>::from_value(Value{Null{}}).ok());
}

TEST(TypeBinder, OptionalDecimalAndTimestampCompose) {
    auto none = TypeBinder<std::optional<halcyon::Decimal>>::from_value(Value{Null{}});
    ASSERT_TRUE(none.ok());
    EXPECT_FALSE(none.value().has_value());
    auto some = TypeBinder<std::optional<halcyon::Timestamp>>::from_value(
        Value{std::string{"2026-06-21 00:00:00"}});
    ASSERT_TRUE(some.ok());
    ASSERT_TRUE(some.value().has_value());
}

// Covers only the binary TypeBinder's pass-through (the neutral Value carries
// bytes verbatim, incl. embedded NULs and >4KB). The driver's chunked
// SQL_C_BINARY read loop (get_binary in db2_cli_driver.cpp) is exercised
// end-to-end by the gated live-Db2 test Db2TypeMapping.BinaryDecimalTemporalRoundTrip.
TEST(TypeBinder, BinaryBinderPassThroughKeepsBytesExact) {
    std::vector<std::byte> blob(5000, std::byte{0xAB});
    blob[10] = std::byte{0x00};  // embedded NUL must not truncate
    blob[4096] = std::byte{0x00};
    auto v = TypeBinder<std::vector<std::byte>>::to_value(blob);
    auto back = TypeBinder<std::vector<std::byte>>::from_value(v);
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value(), blob);  // byte-exact
}

TEST(TypeBinder, NewTypesAreBindableAndReadable) {
    static_assert(halcyon::is_bindable<halcyon::Decimal>::value);
    static_assert(halcyon::is_readable<halcyon::Decimal>::value);
    static_assert(halcyon::is_bindable<halcyon::Timestamp>::value);
    static_assert(halcyon::is_readable<halcyon::Date>::value);
    static_assert(
        halcyon::is_bindable<std::chrono::system_clock::time_point>::value);
    static_assert(
        halcyon::is_readable<std::chrono::system_clock::time_point>::value);
}

// --- std::chrono <-> TIMESTAMP (spec §7) ---

using TimePoint = std::chrono::system_clock::time_point;

TEST(TypeBinder, ChronoTimestampRoundTripsToCanonicalUtcText) {
    auto v = TypeBinder<TimePoint>::from_value(
        Value{std::string{"2026-06-21 13:45:00.123456"}});
    ASSERT_TRUE(v.ok());
    // Re-emit: the canonical UTC form round-trips exactly.
    EXPECT_EQ(TypeBinder<TimePoint>::to_value(v.value()),
              Value{std::string{"2026-06-21 13:45:00.123456"}});
}

TEST(TypeBinder, ChronoParsesDb2DashSeparatedForm) {
    // Db2's default TIMESTAMP text uses '-'/'.' separators; ISO space/colon also.
    auto a = TypeBinder<TimePoint>::from_value(
        Value{std::string{"2026-06-21-13.45.00.123456"}});
    auto b = TypeBinder<TimePoint>::from_value(
        Value{std::string{"2026-06-21 13:45:00.123456"}});
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.value(), b.value());
}

TEST(TypeBinder, ChronoNullAndMismatchAreMappingErrors) {
    EXPECT_FALSE(TypeBinder<TimePoint>::from_value(Value{Null{}}).ok());
    EXPECT_FALSE(TypeBinder<TimePoint>::from_value(Value{std::int64_t{5}}).ok());
}

// v1.2: an already-materialized seam Value binds through the normal variadic
// paths unchanged (identity). Bind-only: reads stay typed.
TEST(TypeBinder, SeamValuePassthroughBindsIdentity) {
    using halcyon::detail::cli::Value;
    static_assert(halcyon::is_bindable<Value>::value,
                  "seam Value must be bindable (v1.2 passthrough)");
    Value s{std::string{"owned"}};
    auto vs = halcyon::detail::to_value(s);
    EXPECT_EQ(std::get<std::string>(vs), "owned");

    Value n{halcyon::detail::cli::Null{}};
    auto vn = halcyon::detail::to_value(n);
    EXPECT_TRUE(std::holds_alternative<halcyon::detail::cli::Null>(vn));

    auto packed = halcyon::detail::pack_params(Value{std::int64_t{42}},
                                               std::string{"mixed"});
    ASSERT_EQ(packed.size(), 2u);
    EXPECT_EQ(std::get<std::int64_t>(packed[0]), 42);
    EXPECT_EQ(std::get<std::string>(packed[1]), "mixed");
}
