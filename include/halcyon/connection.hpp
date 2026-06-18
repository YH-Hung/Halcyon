#pragma once

#include <cstddef>
#include <iterator>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/error.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/result.hpp"
#include "halcyon/types.hpp"

namespace halcyon {

// A non-owning view of the current cursor row. Valid only at the iterator's
// current position (forward-only); reads columns lazily via the seam.
class Row {
public:
    Row(detail::cli::ICliDriver& driver, detail::cli::StatementHandle stmt,
        std::size_t columns)
        : driver_(&driver), stmt_(stmt), columns_(columns) {}

    std::size_t column_count() const noexcept { return columns_; }

    // Result form: maps the row to a tuple<Ts...>; Mapping error on arity or
    // type/null mismatch.
    template <class... Ts>
    Result<std::tuple<Ts...>> try_as() const {
        static_assert((is_readable<Ts>::value && ...),
                      "every column type must have a readable TypeBinder");
        if (sizeof...(Ts) != columns_)
            return detail::mapping_error("row.as<>: column count mismatch");
        return read_tuple<Ts...>(std::index_sequence_for<Ts...>{});
    }

    // Throwing form (OO style): unwraps try_as via Result::value().
    template <class... Ts>
    std::tuple<Ts...> as() const {
        return try_as<Ts...>().value();
    }

private:
    template <class... Ts, std::size_t... I>
    Result<std::tuple<Ts...>> read_tuple(std::index_sequence<I...>) const {
        std::tuple<Ts...> out;
        Error err;
        bool ok = true;
        auto step = [&](auto idx, auto* slot) {
            if (!ok) return;
            constexpr std::size_t i = decltype(idx)::value;
            auto cell = driver_->getColumn(stmt_, i);
            if (!cell.ok()) { ok = false; err = cell.error(); return; }
            using F = std::remove_pointer_t<decltype(slot)>;
            auto v = TypeBinder<F>::from_value(cell.value());
            if (!v.ok()) { ok = false; err = v.error(); return; }
            *slot = std::move(v.value());
        };
        (step(std::integral_constant<std::size_t, I>{}, &std::get<I>(out)), ...);
        if (!ok) return err;
        return out;
    }

    detail::cli::ICliDriver* driver_;
    detail::cli::StatementHandle stmt_;
    std::size_t columns_;
};

// A forward-only result cursor over a statement. May borrow the statement
// (Statement keeps ownership) or own it (created by Connection::query). Single
// active cursor per statement; do not outlive the owning Statement when borrowing.
class ResultSet {
public:
    ResultSet(ResultSet&&) = default;
    ResultSet& operator=(ResultSet&&) = default;
    ResultSet(const ResultSet&) = delete;
    ResultSet& operator=(const ResultSet&) = delete;

    static Result<ResultSet> create_borrowing(detail::cli::ICliDriver& driver,
                                              detail::cli::StatementHandle stmt) {
        auto cc = driver.columnCount(stmt);
        if (!cc.ok()) return cc.error();
        return ResultSet(&driver, stmt, cc.value());
    }

    std::size_t column_count() const noexcept { return columns_; }

    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = Row;
        using difference_type = std::ptrdiff_t;
        using pointer = const Row*;
        using reference = const Row&;

        iterator() = default;  // end sentinel: at_end_ == true
        explicit iterator(ResultSet* rs) : rs_(rs), at_end_(false) { advance(); }

        const Row& operator*() const { return *row_; }
        const Row* operator->() const { return &*row_; }
        iterator& operator++() { advance(); return *this; }
        bool operator==(const iterator& o) const noexcept { return at_end_ == o.at_end_; }
        bool operator!=(const iterator& o) const noexcept { return !(*this == o); }

    private:
        void advance() {
            auto f = rs_->driver_->fetch(rs_->stmt_);
            if (!f.ok() || !f.value()) { at_end_ = true; row_.reset(); return; }
            row_.emplace(*rs_->driver_, rs_->stmt_, rs_->columns_);
        }
        ResultSet* rs_ = nullptr;
        bool at_end_ = true;
        std::optional<Row> row_;
    };

    iterator begin() { return iterator(this); }
    iterator end() { return iterator(); }

private:
    ResultSet(detail::cli::ICliDriver* driver, detail::cli::StatementHandle stmt,
              std::size_t columns)
        : driver_(driver), stmt_(stmt), columns_(columns) {}

    friend class Connection;  // for the owning-ResultSet constructor in Task 5
    detail::cli::ICliDriver* driver_;
    detail::cli::StatementHandle stmt_;
    std::size_t columns_;
};

}  // namespace halcyon
