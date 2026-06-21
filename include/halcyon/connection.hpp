#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/error.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/result.hpp"
#include "halcyon/types.hpp"

namespace halcyon {

class ResultSet;   // fwd
class Transaction;  // fwd (defined in transaction.hpp)

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

// A prepared, reusable statement. Owns its driver statement handle and finalizes
// it on destruction. Move-only.
class Statement {
public:
    Statement(detail::cli::ICliDriver& driver, detail::cli::StatementHandle handle)
        : driver_(&driver), handle_(handle) {}

    Statement(Statement&& o) noexcept
        : driver_(o.driver_), handle_(o.handle_) {
        o.handle_ = detail::cli::StatementHandle::invalid;
    }
    Statement& operator=(Statement&& o) noexcept {
        if (this != &o) {
            reset();
            driver_ = o.driver_;
            handle_ = o.handle_;
            o.handle_ = detail::cli::StatementHandle::invalid;
        }
        return *this;
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement() { reset(); }

    detail::cli::StatementHandle handle() const noexcept { return handle_; }

    // Binds params, executes, and returns a borrowing ResultSet (valid until the
    // next exec on this statement or until *this is destroyed). Defined out of
    // line below, once ResultSet is complete.
    template <class... Args>
    Result<ResultSet> execute_query(const Args&... args);

    template <class... Args>
    Result<std::int64_t> execute_update(const Args&... args) {
        return exec(detail::pack_params(args...));
    }

private:
    Result<std::int64_t> exec(const std::vector<detail::cli::Value>& params) {
        auto b = driver_->bindParams(handle_, params);
        if (!b.ok()) return b.error();
        return driver_->execute(handle_);
    }
    void reset() {
        if (driver_ && handle_ != detail::cli::StatementHandle::invalid) {
            driver_->finalize(handle_);
            handle_ = detail::cli::StatementHandle::invalid;
        }
    }
    detail::cli::ICliDriver* driver_;
    detail::cli::StatementHandle handle_;
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

    // The error that ended iteration early, if any. Forward-only iteration stops
    // when fetch() reports end-of-cursor OR an error; this distinguishes the two
    // so a mid-stream Db2 error is never silently mistaken for end-of-results.
    // Check after a range-for loop: empty == clean end, set == fetch failed.
    const std::optional<Error>& error() const noexcept { return error_; }
    bool ok() const noexcept { return !error_.has_value(); }

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
            if (!f.ok()) {  // a mid-stream Db2 error ends iteration, recorded
                rs_->error_ = f.error();
                at_end_ = true;
                row_.reset();
                return;
            }
            if (!f.value()) {  // clean end of cursor
                at_end_ = true;
                row_.reset();
                return;
            }
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

    friend class Connection;  // for the owning-ResultSet constructor
    detail::cli::ICliDriver* driver_;
    detail::cli::StatementHandle stmt_;
    std::size_t columns_;
    std::optional<Error> error_;      // set if a fetch failed mid-iteration
    std::optional<Statement> owned_;  // present when the ResultSet owns its statement
};

// Out-of-line definition now that ResultSet is complete.
template <class... Args>
Result<ResultSet> Statement::execute_query(const Args&... args) {
    auto pre = exec(detail::pack_params(args...));
    if (!pre.ok()) return pre.error();
    return ResultSet::create_borrowing(*driver_, handle_);
}

// A single logical connection over the seam. Owns the physical connection handle
// and disconnects on destruction. Move-only; not safe for concurrent use by
// multiple threads (matches CLI handle semantics).
class Connection {
public:
    static Result<Connection> open(detail::cli::ICliDriver& driver,
                                   const detail::cli::ConnectionParams& params) {
        auto h = driver.connect(params);
        if (!h.ok()) return h.error();
        return Connection(driver, h.value());
    }

    Connection(detail::cli::ICliDriver& driver, detail::cli::ConnectionHandle handle)
        : driver_(&driver), handle_(handle) {}

    Connection(Connection&& o) noexcept : driver_(o.driver_), handle_(o.handle_) {
        o.handle_ = detail::cli::ConnectionHandle::invalid;
    }
    Connection& operator=(Connection&& o) noexcept {
        if (this != &o) {
            reset();
            driver_ = o.driver_;
            handle_ = o.handle_;
            o.handle_ = detail::cli::ConnectionHandle::invalid;
        }
        return *this;
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection() { reset(); }

    detail::cli::ConnectionHandle handle() const noexcept { return handle_; }

    detail::cli::ICliDriver& driver() const noexcept { return *driver_; }

    Result<Statement> prepare(const std::string& sql) {
        auto h = driver_->prepare(handle_, sql);
        if (!h.ok()) return h.error();
        return Statement(*driver_, h.value());
    }

    // --- anonymous-parameter overloads (enabled only when all Args bind) ---
    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<ResultSet> query(const std::string& sql, const Args&... args) {
        auto st = prepare(sql);
        if (!st.ok()) return st.error();
        return run_query(std::move(st.value()), detail::pack_params(args...));
    }

    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::int64_t> execute(const std::string& sql, const Args&... args) {
        auto st = prepare(sql);
        if (!st.ok()) return st.error();
        return st.value().execute_update(args...);
    }

    // --- named-parameter overloads ---
    Result<ResultSet> query(const std::string& sql, const params& named) {
        auto pre = detail::bind_named(sql, named);
        if (!pre.ok()) return pre.error();
        auto st = prepare(pre.value().sql);
        if (!st.ok()) return st.error();
        return run_query(std::move(st.value()), pre.value().params);
    }

    Result<std::int64_t> execute(const std::string& sql, const params& named) {
        auto pre = detail::bind_named(sql, named);
        if (!pre.ok()) return pre.error();
        auto st = prepare(pre.value().sql);
        if (!st.ok()) return st.error();
        auto b = driver_->bindParams(st.value().handle(), pre.value().params);
        if (!b.ok()) return b.error();
        return driver_->execute(st.value().handle());
    }

    // --- struct-mapping overloads (require HALCYON_REFLECT(T, ...)) ---
    template <class T, class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::vector<T>> queryAs(const std::string& sql, const Args&... args) {
        auto st = prepare(sql);
        if (!st.ok()) return st.error();
        return collect<T>(st.value(), detail::pack_params(args...));
    }

    template <class T>
    Result<std::vector<T>> queryAs(const std::string& sql, const params& named) {
        auto pre = detail::bind_named(sql, named);
        if (!pre.ok()) return pre.error();
        auto st = prepare(pre.value().sql);
        if (!st.ok()) return st.error();
        return collect<T>(st.value(), pre.value().params);
    }

    // Prepares sql once and executes it for each row of positional params,
    // accumulating affected-row counts. Stops at the first error.
    Result<std::int64_t> executeBatch(
        const std::string& sql,
        const std::vector<std::vector<detail::cli::Value>>& rows) {
        if (rows.empty()) return std::int64_t{0};
        auto st = prepare(sql);
        if (!st.ok()) return st.error();
        std::int64_t total = 0;
        for (const auto& row : rows) {
            auto b = driver_->bindParams(st.value().handle(), row);
            if (!b.ok()) return b.error();
            auto e = driver_->execute(st.value().handle());
            if (!e.ok()) return e.error();
            total += e.value();
        }
        return total;
    }

    // Begins a transaction (autocommit OFF). Defined in transaction.hpp.
    Result<Transaction> begin();

private:
    template <class T>
    Result<std::vector<T>> collect(Statement& st,
                                   const std::vector<detail::cli::Value>& params) {
        auto b = driver_->bindParams(st.handle(), params);
        if (!b.ok()) return b.error();
        auto e = driver_->execute(st.handle());
        if (!e.ok()) return e.error();
        auto cc = driver_->columnCount(st.handle());
        if (!cc.ok()) return cc.error();
        std::vector<T> out;
        for (;;) {
            auto f = driver_->fetch(st.handle());
            if (!f.ok()) return f.error();
            if (!f.value()) break;
            auto row = reflect::map_row<T>(*driver_, st.handle(), cc.value());
            if (!row.ok()) return row.error();
            out.push_back(std::move(row.value()));
        }
        return out;
    }

    // Builds a ResultSet that OWNS the statement (kept alive for the cursor).
    Result<ResultSet> run_query(Statement&& st,
                                const std::vector<detail::cli::Value>& params) {
        auto b = driver_->bindParams(st.handle(), params);
        if (!b.ok()) return b.error();
        auto e = driver_->execute(st.handle());
        if (!e.ok()) return e.error();
        auto cc = driver_->columnCount(st.handle());
        if (!cc.ok()) return cc.error();
        ResultSet rs(driver_, st.handle(), cc.value());
        rs.owned_ = std::move(st);
        return rs;
    }

    void reset() {
        if (driver_ && handle_ != detail::cli::ConnectionHandle::invalid) {
            driver_->disconnect(handle_);
            handle_ = detail::cli::ConnectionHandle::invalid;
        }
    }
    detail::cli::ICliDriver* driver_;
    detail::cli::ConnectionHandle handle_;
};

}  // namespace halcyon
