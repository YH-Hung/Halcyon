#pragma once

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/error.hpp"
#include "halcyon/result.hpp"

namespace halcyon {

namespace detail {

inline Error mapping_error(std::string message) {
    Error e;
    e.code = ErrorCode::Mapping;
    e.message = std::move(message);
    return e;
}

}  // namespace detail

// Primary template is declared (not defined): a missing specialization yields a
// clear "incomplete type" error and disables the traits below via SFINAE.
template <class T, class Enable = void>
struct TypeBinder;

// --- integral (excluding bool) ---
template <class T>
struct TypeBinder<T, std::enable_if_t<std::is_integral_v<T> &&
                                      !std::is_same_v<T, bool>>> {
    static detail::cli::Value to_value(T v) {
        return detail::cli::Value{static_cast<std::int64_t>(v)};
    }
    static Result<T> from_value(const detail::cli::Value& v) {
        if (std::holds_alternative<detail::cli::Null>(v))
            return detail::mapping_error("NULL into non-optional integral");
        if (auto* p = std::get_if<std::int64_t>(&v)) {
            if (!in_range(*p))
                return detail::mapping_error("integer out of range for target type");
            return static_cast<T>(*p);
        }
        if (auto* b = std::get_if<bool>(&v)) return static_cast<T>(*b ? 1 : 0);
        return detail::mapping_error("type mismatch: expected integer");
    }

private:
    static bool in_range(std::int64_t v) {
        using L = std::numeric_limits<T>;
        if constexpr (std::is_signed_v<T>) {
            return v >= static_cast<std::int64_t>(L::min()) &&
                   v <= static_cast<std::int64_t>(L::max());
        } else {
            return v >= 0 &&
                   static_cast<std::uint64_t>(v) <=
                       static_cast<std::uint64_t>(L::max());
        }
    }
};

// --- bool ---
template <>
struct TypeBinder<bool> {
    static detail::cli::Value to_value(bool v) { return detail::cli::Value{v}; }
    static Result<bool> from_value(const detail::cli::Value& v) {
        if (auto* b = std::get_if<bool>(&v)) return *b;
        if (auto* i = std::get_if<std::int64_t>(&v)) return *i != 0;
        if (std::holds_alternative<detail::cli::Null>(v))
            return detail::mapping_error("NULL into non-optional bool");
        return detail::mapping_error("type mismatch: expected bool");
    }
};

// --- floating point ---
template <class T>
struct TypeBinder<T, std::enable_if_t<std::is_floating_point_v<T>>> {
    static detail::cli::Value to_value(T v) {
        return detail::cli::Value{static_cast<double>(v)};
    }
    static Result<T> from_value(const detail::cli::Value& v) {
        if (auto* d = std::get_if<double>(&v)) return static_cast<T>(*d);
        if (auto* i = std::get_if<std::int64_t>(&v)) return static_cast<T>(*i);
        if (std::holds_alternative<detail::cli::Null>(v))
            return detail::mapping_error("NULL into non-optional floating point");
        return detail::mapping_error("type mismatch: expected floating point");
    }
};

// --- std::string ---
template <>
struct TypeBinder<std::string> {
    static detail::cli::Value to_value(std::string v) {
        return detail::cli::Value{std::move(v)};
    }
    static Result<std::string> from_value(const detail::cli::Value& v) {
        if (auto* s = std::get_if<std::string>(&v)) return *s;
        if (std::holds_alternative<detail::cli::Null>(v))
            return detail::mapping_error("NULL into non-optional string");
        return detail::mapping_error("type mismatch: expected string");
    }
};

// --- std::vector<std::byte> (binary) ---
template <>
struct TypeBinder<std::vector<std::byte>> {
    static detail::cli::Value to_value(std::vector<std::byte> v) {
        return detail::cli::Value{std::move(v)};
    }
    static Result<std::vector<std::byte>> from_value(const detail::cli::Value& v) {
        if (auto* b = std::get_if<std::vector<std::byte>>(&v)) return *b;
        if (std::holds_alternative<detail::cli::Null>(v))
            return detail::mapping_error("NULL into non-optional binary");
        return detail::mapping_error("type mismatch: expected binary");
    }
};

// --- seam Value passthrough (bind-only, v1.2) ---
// Lets an already-materialized detail::cli::Value be passed wherever a
// bindable parameter is accepted (identity). The coroutine layer's factories
// materialize arguments into owned Values at the call site and re-bind them
// later through this passthrough. Bind-only: read typed values instead.
template <>
struct TypeBinder<detail::cli::Value> {
    static detail::cli::Value to_value(detail::cli::Value v) { return v; }
};

// --- bind-only std::string_view ---
template <>
struct TypeBinder<std::string_view> {
    static detail::cli::Value to_value(std::string_view v) {
        return detail::cli::Value{std::string{v}};
    }
    // intentionally no from_value: read into std::string instead.
};

// --- std::optional<T> (nullability) ---
template <class U>
struct TypeBinder<std::optional<U>> {
    static detail::cli::Value to_value(const std::optional<U>& o) {
        if (o) return TypeBinder<U>::to_value(*o);
        return detail::cli::Value{detail::cli::Null{}};
    }
    static Result<std::optional<U>> from_value(const detail::cli::Value& v) {
        if (std::holds_alternative<detail::cli::Null>(v))
            return std::optional<U>{};
        auto r = TypeBinder<U>::from_value(v);
        if (!r.ok()) return r.error();
        return std::optional<U>{std::move(r.value())};
    }
};

// --- exact decimal + date/time (spec §7) ---
// These cross the thin CLI seam as text: the neutral Value variant carries no
// temporal/decimal alternative, and Db2 round-trips DECIMAL and DATE/TIME/
// TIMESTAMP losslessly as SQL_C_CHAR. Each wrapper stores the driver's text
// verbatim, so exact scale and sub-second precision are preserved.

// Exact decimal, stored as its textual form (e.g. "123.4500").
class Decimal {
public:
    Decimal() = default;
    explicit Decimal(std::string text) : text_(std::move(text)) {}
    const std::string& str() const noexcept { return text_; }
    friend bool operator==(const Decimal& a, const Decimal& b) {
        return a.text_ == b.text_;
    }
    friend bool operator!=(const Decimal& a, const Decimal& b) {
        return !(a == b);
    }

private:
    std::string text_;
};

// SQL DATE / TIME / TIMESTAMP as text (ISO-8601-ish; the exact form is whatever
// the driver emits). Distinct types so binding/reading stays type-safe.
struct Date {
    std::string value;  // e.g. "2026-06-21"
};
struct Time {
    std::string value;  // e.g. "13:45:00"
};
struct Timestamp {
    std::string value;  // e.g. "2026-06-21 13:45:00.123456"
};
inline bool operator==(const Date& a, const Date& b) { return a.value == b.value; }
inline bool operator!=(const Date& a, const Date& b) { return !(a == b); }
inline bool operator==(const Time& a, const Time& b) { return a.value == b.value; }
inline bool operator!=(const Time& a, const Time& b) { return !(a == b); }
inline bool operator==(const Timestamp& a, const Timestamp& b) {
    return a.value == b.value;
}
inline bool operator!=(const Timestamp& a, const Timestamp& b) {
    return !(a == b);
}

namespace detail {
// Shared from_value for a string-carried wrapper type: NULL and wrong-alternative
// both become Mapping errors; a string alternative is handed to make().
template <class Make>
auto string_carried_from_value(const detail::cli::Value& v, const char* what,
                               Make make)
    -> Result<decltype(make(std::declval<const std::string&>()))> {
    using T = decltype(make(std::declval<const std::string&>()));
    if (std::holds_alternative<detail::cli::Null>(v))
        return mapping_error(std::string("NULL into non-optional ") + what);
    if (auto* s = std::get_if<std::string>(&v)) return T{make(*s)};
    return mapping_error(std::string("type mismatch: expected ") + what);
}
}  // namespace detail

template <>
struct TypeBinder<Decimal> {
    static detail::cli::Value to_value(const Decimal& d) {
        return detail::cli::Value{d.str()};
    }
    static Result<Decimal> from_value(const detail::cli::Value& v) {
        return detail::string_carried_from_value(
            v, "decimal", [](const std::string& s) { return Decimal{s}; });
    }
};

template <>
struct TypeBinder<Date> {
    static detail::cli::Value to_value(const Date& d) {
        return detail::cli::Value{d.value};
    }
    static Result<Date> from_value(const detail::cli::Value& v) {
        return detail::string_carried_from_value(
            v, "date", [](const std::string& s) { return Date{s}; });
    }
};

template <>
struct TypeBinder<Time> {
    static detail::cli::Value to_value(const Time& t) {
        return detail::cli::Value{t.value};
    }
    static Result<Time> from_value(const detail::cli::Value& v) {
        return detail::string_carried_from_value(
            v, "time", [](const std::string& s) { return Time{s}; });
    }
};

template <>
struct TypeBinder<Timestamp> {
    static detail::cli::Value to_value(const Timestamp& t) {
        return detail::cli::Value{t.value};
    }
    static Result<Timestamp> from_value(const detail::cli::Value& v) {
        return detail::string_carried_from_value(
            v, "timestamp", [](const std::string& s) { return Timestamp{s}; });
    }
};

// --- std::chrono interop (spec §7: std::chrono <-> SQL_C_TYPE_TIMESTAMP) ---
// system_clock::time_point is the one C++17 chrono type that maps cleanly to a
// SQL TIMESTAMP (bare DATE/TIME have no epoch and C++17 has no calendar types,
// so those keep the string-exact halcyon::Date/Time wrappers above). The
// time_point is carried over the seam as a UTC ISO-8601 string (SQL_C_CHAR),
// which Db2 implicitly casts to/from TIMESTAMP.
namespace detail {

// Howard Hinnant's calendar algorithms (public domain): days since 1970-01-01.
constexpr long long days_from_civil(long long y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m > 2 ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097LL + static_cast<long long>(doe) - 719468LL;
}

struct civil_date {
    long long y;
    unsigned m;
    unsigned d;
};
constexpr civil_date civil_from_days(long long z) noexcept {
    z += 719468LL;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const long long y = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned m = mp < 10u ? mp + 3u : mp - 9u;
    return civil_date{y + (m <= 2), m, d};
}

inline std::string format_timestamp(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    const auto secs = floor<seconds>(tp);  // secs <= tp, so frac is in [0, 1e6)
    const long long micros = duration_cast<microseconds>(tp - secs).count();
    long long s = secs.time_since_epoch().count();
    long long days = s / 86400;
    long long rem = s % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    const civil_date c = civil_from_days(days);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u %02lld:%02lld:%02lld.%06lld",
                  c.y, c.m, c.d, rem / 3600, (rem % 3600) / 60, rem % 60, micros);
    return std::string(buf);
}

// Tolerant parse of a Db2 timestamp string: splits on any non-digit, so both
// "YYYY-MM-DD HH:MM:SS.ffffff" and Db2's "YYYY-MM-DD-HH.MM.SS.ffffff" parse. The
// fractional field is scaled to microseconds (system_clock's practical limit).
inline Result<std::chrono::system_clock::time_point> parse_timestamp(
    const std::string& s) {
    long long parts[7] = {0, 0, 0, 0, 0, 0, 0};
    int np = 0;
    std::size_t frac_digits = 0;
    for (std::size_t i = 0; i < s.size() && np < 7;) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
            ++i;
            continue;
        }
        const std::size_t start = i;
        long long val = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            val = val * 10 + (s[i] - '0');
            ++i;
        }
        if (np == 6) frac_digits = i - start;
        parts[np++] = val;
    }
    if (np < 6)
        return mapping_error("invalid timestamp text: '" + s + "'");
    long long micros = np >= 7 ? parts[6] : 0;
    for (std::size_t k = frac_digits; k > 6; --k) micros /= 10;             // trim > microsec
    for (std::size_t k = frac_digits; k < 6 && np >= 7; ++k) micros *= 10;  // pad
    const long long days =
        days_from_civil(parts[0], static_cast<unsigned>(parts[1]),
                        static_cast<unsigned>(parts[2]));
    const long long secs = days * 86400LL + parts[3] * 3600 + parts[4] * 60 + parts[5];
    using namespace std::chrono;
    return system_clock::time_point(seconds(secs) + microseconds(micros));
}

}  // namespace detail

template <>
struct TypeBinder<std::chrono::system_clock::time_point> {
    using TP = std::chrono::system_clock::time_point;
    static detail::cli::Value to_value(TP tp) {
        return detail::cli::Value{detail::format_timestamp(tp)};
    }
    static Result<TP> from_value(const detail::cli::Value& v) {
        if (std::holds_alternative<detail::cli::Null>(v))
            return detail::mapping_error("NULL into non-optional timestamp");
        if (auto* s = std::get_if<std::string>(&v))
            return detail::parse_timestamp(*s);
        return detail::mapping_error("type mismatch: expected timestamp string");
    }
};

// --- traits ---
template <class T, class Enable = void>
struct is_bindable : std::false_type {};
template <class T>
struct is_bindable<T, std::void_t<decltype(TypeBinder<std::decay_t<T>>::to_value(
                          std::declval<const std::decay_t<T>&>()))>>
    : std::true_type {};

template <class T, class Enable = void>
struct is_readable : std::false_type {};
template <class T>
struct is_readable<T, std::void_t<decltype(TypeBinder<std::decay_t<T>>::from_value(
                          std::declval<const detail::cli::Value&>()))>>
    : std::true_type {};

namespace detail {

// Bind helper that also accepts string literals / string_view by value.
template <class T>
detail::cli::Value to_value(const T& v) {
    using D = std::decay_t<T>;
    if constexpr (std::is_convertible_v<const T&, std::string_view> &&
                  !std::is_same_v<D, std::string>) {
        return detail::cli::Value{std::string{std::string_view(v)}};
    } else {
        return TypeBinder<D>::to_value(v);
    }
}

template <class T>
Result<std::decay_t<T>> from_value(const detail::cli::Value& v) {
    return TypeBinder<std::decay_t<T>>::from_value(v);
}

}  // namespace detail

}  // namespace halcyon

namespace halcyon {
namespace reflect {

// Primary: a type is not reflected unless HALCYON_REFLECT specializes this.
template <class T>
struct Reflected : std::false_type {
    static constexpr std::size_t field_count = 0;
};

// Assigns each column (by position) into the struct via its member pointers.
template <class T, class Tuple, std::size_t... I>
Result<void> assign_fields(T& out, const Tuple& mptrs,
                           const std::vector<detail::cli::Value>& cells,
                           std::index_sequence<I...>) {
    Error err;
    bool ok = true;
    auto step = [&](auto idx) {
        if (!ok) return;
        constexpr std::size_t i = decltype(idx)::value;
        auto mp = std::get<i>(mptrs);
        using F = std::remove_reference_t<decltype(out.*mp)>;
        auto v = TypeBinder<F>::from_value(cells[i]);
        if (!v.ok()) {
            ok = false;
            err = v.error();
            return;
        }
        out.*mp = std::move(v.value());
    };
    (step(std::integral_constant<std::size_t, I>{}), ...);
    if (!ok) return err;
    return Result<void>();
}

// Maps an already-materialized row into a freshly value-initialized T.
template <class T>
Result<T> map_row(const std::vector<detail::cli::Value>& cells) {
    static_assert(Reflected<T>::value,
                  "queryAs<T> requires HALCYON_REFLECT(T, fields...)");
    constexpr std::size_t N = Reflected<T>::field_count;
    if (cells.size() != N)
        return detail::mapping_error("queryAs<T>: column count != field count");
    auto mptrs = Reflected<T>::members();
    T out{};
    auto r = assign_fields(out, mptrs, cells, std::make_index_sequence<N>{});
    if (!r.ok()) return r.error();
    return out;
}

}  // namespace reflect
}  // namespace halcyon

// ---- HALCYON_REFLECT macro (global scope) ----
// Expands field names to a tuple of member pointers and specializes Reflected<T>.
#define HALCYON_PP_EXPAND(x) x
#define HALCYON_PP_CAT(a, b) HALCYON_PP_CAT_(a, b)
#define HALCYON_PP_CAT_(a, b) a##b

#define HALCYON_PP_NARG(...) \
    HALCYON_PP_EXPAND(HALCYON_PP_NARG_(__VA_ARGS__, HALCYON_PP_RSEQ()))
#define HALCYON_PP_NARG_(...) HALCYON_PP_EXPAND(HALCYON_PP_ARG_N(__VA_ARGS__))
#define HALCYON_PP_ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, N, ...) N
#define HALCYON_PP_RSEQ() 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0

#define HALCYON_MP(T, field) &T::field
#define HALCYON_FE_1(T, a) HALCYON_MP(T, a)
#define HALCYON_FE_2(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_1(T, __VA_ARGS__))
#define HALCYON_FE_3(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_2(T, __VA_ARGS__))
#define HALCYON_FE_4(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_3(T, __VA_ARGS__))
#define HALCYON_FE_5(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_4(T, __VA_ARGS__))
#define HALCYON_FE_6(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_5(T, __VA_ARGS__))
#define HALCYON_FE_7(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_6(T, __VA_ARGS__))
#define HALCYON_FE_8(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_7(T, __VA_ARGS__))
#define HALCYON_FE_9(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_8(T, __VA_ARGS__))
#define HALCYON_FE_10(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_9(T, __VA_ARGS__))
#define HALCYON_MEMBER_PTRS(T, ...) \
    HALCYON_PP_EXPAND(HALCYON_PP_CAT(HALCYON_FE_, HALCYON_PP_NARG(__VA_ARGS__))(T, __VA_ARGS__))

// Supports up to 10 fields; extend the HALCYON_FE_<n>/HALCYON_PP_* ladder for more.
#define HALCYON_REFLECT(Type, ...)                                               \
    template <>                                                                  \
    struct halcyon::reflect::Reflected<Type> : std::true_type {                  \
        static constexpr std::size_t field_count = HALCYON_PP_NARG(__VA_ARGS__); \
        static auto members() {                                                  \
            return std::make_tuple(HALCYON_MEMBER_PTRS(Type, __VA_ARGS__));      \
        }                                                                        \
    }
