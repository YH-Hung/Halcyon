#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "halcyon/detail/batch_validate.hpp"
#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/detail/statement_cache.hpp"
#include "halcyon/error.hpp"
#include "halcyon/observability/metrics.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/result.hpp"
#include "halcyon/types.hpp"

namespace halcyon {

class ResultSet;    // fwd
class Transaction;  // fwd (defined in transaction.hpp)

/// \brief Non-owning view of the current cursor row; valid only at the iterator's position.
///
/// Read columns via `as<Ts...>()` (throws on mismatch) or `try_as<Ts...>()` (Result form).
/// Valid only during forward iteration of the owning `ResultSet`.
class Row {
public:
    // owner is the streaming ResultSet this row belongs to (null for a Row not
    // backed by one, e.g. map_row's direct reads). When set, a getColumn failure
    // is recorded on it so a mid-stream connection drop during SQLGetData can be
    // detected and the connection discarded rather than returned to the pool.
    Row(detail::cli::ICliDriver& driver, detail::cli::StatementHandle stmt,
        std::size_t columns, ResultSet* owner = nullptr)
        : driver_(&driver), stmt_(stmt), columns_(columns), owner_(owner) {}

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
            if (!cell.ok()) {
                ok = false;
                err = cell.error();
                // A driver-side read failure (e.g. connection drop during
                // SQLGetData) is recorded on the owning ResultSet so the cursor's
                // lease/connection can be discarded — a from_value mapping error
                // below is client-side and deliberately left untouched.
                record_read_error(err);
                return;
            }
            using F = std::remove_pointer_t<decltype(slot)>;
            auto v = TypeBinder<F>::from_value(cell.value());
            if (!v.ok()) {
                ok = false;
                err = v.error();
                return;
            }
            *slot = std::move(v.value());
        };
        (step(std::integral_constant<std::size_t, I>{}, &std::get<I>(out)), ...);
        if (!ok) return err;
        return out;
    }

    // Records a column-read failure on the owning ResultSet (if any). Defined out
    // of line below, once ResultSet is complete.
    void record_read_error(const Error& e) const;

    detail::cli::ICliDriver* driver_;
    detail::cli::StatementHandle stmt_;
    std::size_t columns_;
    ResultSet* owner_ = nullptr;
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

/// \brief Forward-only result cursor over a prepared statement.
///
/// Iterate with a range-for loop yielding `Row` objects. After the loop, check
/// `ok()`/`error()` to distinguish a clean end from a mid-stream fetch failure.
/// Move-only; a single active cursor per statement.
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

    // Records a column-read failure (from Row::try_as) so a mid-stream driver
    // error during SQLGetData is treated like a fetch failure: the first error is
    // kept, and the cached statement is poisoned so its lease is finalized +
    // dropped on release. The facade's QueryResult then discards a connection-
    // class error's connection instead of returning it to the pool.
    void note_column_error(const Error& e) {
        if (!error_) error_ = e;
        if (lease_) lease_->poison();
    }

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
        iterator& operator++() {
            advance();
            return *this;
        }
        bool operator==(const iterator& o) const noexcept { return at_end_ == o.at_end_; }
        bool operator!=(const iterator& o) const noexcept { return !(*this == o); }

    private:
        void advance() {
            auto f = rs_->driver_->fetch(rs_->stmt_);
            if (!f.ok()) {  // a mid-stream Db2 error ends iteration, recorded
                rs_->error_ = f.error();
                // A fetch error on a cached handle may leave it in a bad state;
                // poison the lease so on release the entry is finalized + dropped
                // and the next call re-prepares (spec §5). No-op for borrowing
                // ResultSets, whose lease_ is empty.
                if (rs_->lease_) rs_->lease_->poison();
                at_end_ = true;
                row_.reset();
                return;
            }
            if (!f.value()) {  // clean end of cursor
                at_end_ = true;
                row_.reset();
                return;
            }
            row_.emplace(*rs_->driver_, rs_->stmt_, rs_->columns_, rs_);
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
    friend class Row;         // for note_column_error via Row::record_read_error
    detail::cli::ICliDriver* driver_;
    detail::cli::StatementHandle stmt_;
    std::size_t columns_;
    std::optional<Error> error_;                   // set if a fetch failed mid-iteration
    std::optional<detail::StatementLease> lease_;  // owns the cached/transient stmt
};

// Out-of-line now that ResultSet is complete: forward a column-read failure to
// the owning streaming ResultSet (no-op for a Row without one).
inline void Row::record_read_error(const Error& e) const {
    if (owner_) owner_->note_column_error(e);
}

// Out-of-line definition now that ResultSet is complete.
template <class... Args>
Result<ResultSet> Statement::execute_query(const Args&... args) {
    auto pre = exec(detail::pack_params(args...));
    if (!pre.ok()) return pre.error();
    return ResultSet::create_borrowing(*driver_, handle_);
}

/// \brief Single logical Db2 connection over the CLI seam.
///
/// Owns the physical connection handle; disconnects on destruction. Move-only
/// and not safe for concurrent use by multiple threads. Normally obtained via
/// `Database` (which leases connections from its `ConnectionPool`).
class Connection {
public:
    static Result<Connection> open(detail::cli::ICliDriver& driver,
                                   const detail::cli::ConnectionParams& params,
                                   std::size_t statementCacheSize = 0,
                                   obs::MetricsSink* metrics = nullptr) {
        auto h = driver.connect(params);
        if (!h.ok()) return h.error();
        return Connection(driver, h.value(), statementCacheSize, metrics);
    }

    Connection(detail::cli::ICliDriver& driver,
               detail::cli::ConnectionHandle handle,
               std::size_t statementCacheSize = 0,
               obs::MetricsSink* metrics = nullptr)
        : driver_(&driver),
          handle_(handle),
          cache_(std::make_unique<detail::StatementCache>(
              driver, handle, statementCacheSize, metrics)) {}

    Connection(Connection&& o) noexcept
        : driver_(o.driver_), handle_(o.handle_), cache_(std::move(o.cache_)) {
        o.handle_ = detail::cli::ConnectionHandle::invalid;
    }
    Connection& operator=(Connection&& o) noexcept {
        if (this != &o) {
            reset();
            driver_ = o.driver_;
            handle_ = o.handle_;
            cache_ = std::move(o.cache_);
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
        auto lease = cache_->acquire(sql);
        if (!lease.ok()) return lease.error();
        return run_query(std::move(lease.value()), detail::pack_params(args...));
    }

    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::int64_t> execute(const std::string& sql, const Args&... args) {
        auto lease = cache_->acquire(sql);
        if (!lease.ok()) return lease.error();
        return exec_lease(lease.value(), detail::pack_params(args...));
    }

    // --- named-parameter overloads ---
    Result<ResultSet> query(const std::string& sql, const params& named) {
        auto pre = detail::bind_named(sql, named);
        if (!pre.ok()) return pre.error();
        auto lease = cache_->acquire(pre.value().sql);
        if (!lease.ok()) return lease.error();
        return run_query(std::move(lease.value()), pre.value().params);
    }

    Result<std::int64_t> execute(const std::string& sql, const params& named) {
        auto pre = detail::bind_named(sql, named);
        if (!pre.ok()) return pre.error();
        auto lease = cache_->acquire(pre.value().sql);
        if (!lease.ok()) return lease.error();
        return exec_lease(lease.value(), pre.value().params);
    }

    // --- struct-mapping overloads (require HALCYON_REFLECT(T, ...)) ---
    template <class T, class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::vector<T>> queryAs(const std::string& sql, const Args&... args) {
        auto lease = cache_->acquire(sql);
        if (!lease.ok()) return lease.error();
        return collect<T>(lease.value(), detail::pack_params(args...));
    }

    template <class T>
    Result<std::vector<T>> queryAs(const std::string& sql, const params& named) {
        auto pre = detail::bind_named(sql, named);
        if (!pre.ok()) return pre.error();
        auto lease = cache_->acquire(pre.value().sql);
        if (!lease.ok()) return lease.error();
        return collect<T>(lease.value(), pre.value().params);
    }

    // Validates the batch (rectangular + type-homogeneous) then delegates to the
    // driver's array-binding executeBatch. Returns total rows affected.
    Result<std::int64_t> executeBatch(
        const std::string& sql,
        const std::vector<std::vector<detail::cli::Value>>& rows) {
        if (rows.empty()) return std::int64_t{0};
        if (auto v = detail::validate_batch_rows(rows); !v.ok())
            return v.error();
        auto lease = cache_->acquire(sql);
        if (!lease.ok()) return lease.error();
        auto e = driver_->executeBatch(lease.value().handle(), rows);
        if (!e.ok()) {
            lease.value().poison();
            return e.error();
        }
        return e.value();
    }

    // Begins a transaction (autocommit OFF). Defined in transaction.hpp.
    Result<Transaction> begin();

private:
    Result<std::int64_t> exec_lease(
        detail::StatementLease& lease,
        const std::vector<detail::cli::Value>& params) {
        auto b = driver_->bindParams(lease.handle(), params);
        if (!b.ok()) {
            lease.poison();
            return b.error();
        }
        auto e = driver_->execute(lease.handle());
        if (!e.ok()) {
            lease.poison();
            return e.error();
        }
        return e.value();
    }

    template <class T>
    Result<std::vector<T>> collect(
        detail::StatementLease& lease,
        const std::vector<detail::cli::Value>& params) {
        auto b = driver_->bindParams(lease.handle(), params);
        if (!b.ok()) {
            lease.poison();
            return b.error();
        }
        auto e = driver_->execute(lease.handle());
        if (!e.ok()) {
            lease.poison();
            return e.error();
        }
        auto cc = driver_->columnCount(lease.handle());
        if (!cc.ok()) {
            lease.poison();
            return cc.error();
        }
        std::vector<T> out;
        for (;;) {
            auto f = driver_->fetch(lease.handle());
            if (!f.ok()) {
                lease.poison();
                return f.error();
            }
            if (!f.value()) break;
            auto row = reflect::map_row<T>(*driver_, lease.handle(), cc.value());
            if (!row.ok()) {
                lease.poison();
                return row.error();
            }
            out.push_back(std::move(row.value()));
        }
        return out;
    }

    // Builds a ResultSet that OWNS the lease (kept alive for the cursor's life;
    // the lease closes the cursor and returns the statement to the cache on
    // destruction).
    Result<ResultSet> run_query(detail::StatementLease&& lease,
                                const std::vector<detail::cli::Value>& params) {
        auto b = driver_->bindParams(lease.handle(), params);
        if (!b.ok()) {
            lease.poison();
            return b.error();
        }
        auto e = driver_->execute(lease.handle());
        if (!e.ok()) {
            lease.poison();
            return e.error();
        }
        auto cc = driver_->columnCount(lease.handle());
        if (!cc.ok()) {
            lease.poison();
            return cc.error();
        }
        ResultSet rs(driver_, lease.handle(), cc.value());
        rs.lease_ = std::move(lease);
        return rs;
    }

    void reset() {
        // Finalize cached statement handles BEFORE dropping the connection: the
        // driver's disconnect frees the connection handle, after which the
        // cache's statement handles would be invalid to finalize.
        cache_.reset();
        if (driver_ && handle_ != detail::cli::ConnectionHandle::invalid) {
            driver_->disconnect(handle_);
            handle_ = detail::cli::ConnectionHandle::invalid;
        }
    }
    detail::cli::ICliDriver* driver_;
    detail::cli::ConnectionHandle handle_;
    std::unique_ptr<detail::StatementCache> cache_;
};

}  // namespace halcyon
