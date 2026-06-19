#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "halcyon/connection.hpp"
#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/error.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/pool.hpp"
#include "halcyon/result.hpp"
#include "halcyon/transaction.hpp"
#include "halcyon/types.hpp"

namespace halcyon {

// Owns a leased connection together with the cursor opened on it, so the row
// range stays valid for the QueryResult's lifetime. Move-only. The facade is the
// only layer permitted to couple a PooledConnection (pool) with a ResultSet
// (core); declaration order matters — lease_ is destroyed AFTER rs_.
class QueryResult {
public:
    QueryResult(QueryResult&&) noexcept = default;
    QueryResult& operator=(QueryResult&&) noexcept = default;
    QueryResult(const QueryResult&) = delete;
    QueryResult& operator=(const QueryResult&) = delete;

    ResultSet& rows() noexcept { return rs_; }
    ResultSet::iterator begin() { return rs_.begin(); }
    ResultSet::iterator end() { return rs_.end(); }
    std::size_t column_count() const noexcept { return rs_.column_count(); }

private:
    friend class Database;
    QueryResult(PooledConnection lease, ResultSet rs)
        : lease_(std::move(lease)), rs_(std::move(rs)) {}

    PooledConnection lease_;  // declared first → destroyed last (after rs_)
    ResultSet rs_;
};

// High-level thread-safe entry point. Copyable handle: copies share one pool
// (shared_ptr), so a Database can be passed by value across threads. Each call
// acquires a pooled connection, runs, and releases it on return.
class Database {
public:
    static Result<Database> open(detail::cli::ICliDriver& driver,
                                 const std::string& dsn, PoolConfig config = {}) {
        auto pool = ConnectionPool::create(driver, {dsn}, std::move(config));
        if (!pool.ok()) return pool.error();
        return Database(std::shared_ptr<ConnectionPool>(std::move(pool.value())));
    }

    static Database openOrThrow(detail::cli::ICliDriver& driver,
                                const std::string& dsn, PoolConfig config = {}) {
        return open(driver, dsn, std::move(config)).value();
    }

    ConnectionPool& pool() noexcept { return *pool_; }

    // --- execute ---
    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::int64_t> execute(const std::string& sql, const Args&... args) {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        return lease.value()->execute(sql, args...);
    }
    Result<std::int64_t> execute(const std::string& sql, const params& named) {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        return lease.value()->execute(sql, named);
    }

    // --- query (returns a lease-owning QueryResult) ---
    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<QueryResult> query(const std::string& sql, const Args&... args) {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        auto rs = lease.value()->query(sql, args...);
        if (!rs.ok()) return rs.error();
        return QueryResult(std::move(lease.value()), std::move(rs.value()));
    }
    Result<QueryResult> query(const std::string& sql, const params& named) {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        auto rs = lease.value()->query(sql, named);
        if (!rs.ok()) return rs.error();
        return QueryResult(std::move(lease.value()), std::move(rs.value()));
    }

    // --- queryAs (materialized; no lease lifetime concern) ---
    template <class T, class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::vector<T>> queryAs(const std::string& sql, const Args&... args) {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        return lease.value()->template queryAs<T>(sql, args...);
    }
    template <class T>
    Result<std::vector<T>> queryAs(const std::string& sql, const params& named) {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        return lease.value()->template queryAs<T>(sql, named);
    }

    // --- throwing overloads ---
    template <class... Args>
    QueryResult queryOrThrow(const std::string& sql, const Args&... args) {
        return query(sql, args...).value();
    }
    QueryResult queryOrThrow(const std::string& sql, const params& named) {
        return query(sql, named).value();
    }
    template <class... Args>
    std::int64_t executeOrThrow(const std::string& sql, const Args&... args) {
        return execute(sql, args...).value();
    }
    std::int64_t executeOrThrow(const std::string& sql, const params& named) {
        return execute(sql, named).value();
    }
    template <class T, class... Args>
    std::vector<T> queryAsOrThrow(const std::string& sql, const Args&... args) {
        return queryAs<T>(sql, args...).value();
    }

private:
    explicit Database(std::shared_ptr<ConnectionPool> pool)
        : pool_(std::move(pool)) {}

    std::shared_ptr<ConnectionPool> pool_;
};

}  // namespace halcyon
