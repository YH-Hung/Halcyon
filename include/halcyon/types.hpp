#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
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
