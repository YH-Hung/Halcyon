#pragma once

#include <type_traits>
#include <utility>
#include <variant>

#include "halcyon/error.hpp"

namespace halcyon {

template <class T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}      // NOLINT(google-explicit-constructor)
    Result(Error err) : data_(std::move(err)) {}      // NOLINT(google-explicit-constructor)

    bool ok() const noexcept { return std::holds_alternative<T>(data_); }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & {
        ensure_ok();
        return std::get<T>(data_);
    }
    const T& value() const& {
        ensure_ok();
        return std::get<T>(data_);
    }
    T&& value() && {
        ensure_ok();
        return std::get<T>(std::move(data_));
    }

    const Error& error() const& { return std::get<Error>(data_); }

    T value_or(T fallback) const& { return ok() ? std::get<T>(data_) : fallback; }

    template <class F>
    auto map(F&& f) const& -> Result<std::decay_t<std::invoke_result_t<F, const T&>>> {
        using U = std::decay_t<std::invoke_result_t<F, const T&>>;
        if (ok()) return Result<U>(std::forward<F>(f)(std::get<T>(data_)));
        return Result<U>(std::get<Error>(data_));
    }

    template <class F>
    auto and_then(F&& f) const& -> std::invoke_result_t<F, const T&> {
        using R = std::invoke_result_t<F, const T&>;
        if (ok()) return std::forward<F>(f)(std::get<T>(data_));
        return R(std::get<Error>(data_));
    }

private:
    void ensure_ok() const {
        if (!ok()) throw_error(std::get<Error>(data_));
    }
    std::variant<T, Error> data_;
};

template <>
class Result<void> {
public:
    Result() : error_(), ok_(true) {}
    Result(Error err) : error_(std::move(err)), ok_(false) {}  // NOLINT

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    void value() const {
        if (!ok_) throw_error(error_);
    }

    const Error& error() const& { return error_; }

private:
    Error error_;
    bool ok_;
};

}  // namespace halcyon
