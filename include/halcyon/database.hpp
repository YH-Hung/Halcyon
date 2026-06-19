#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "halcyon/connection.hpp"
#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/error.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/pool.hpp"
#include "halcyon/result.hpp"
#include "halcyon/retry.hpp"
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
        return run_with_policy(default_policy(sql), [&](Connection& c) {
            return c.execute(sql, args...);
        });
    }
    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::int64_t> execute(const std::string& sql, const ExecPolicy& policy,
                                 const Args&... args) {
        return run_with_policy(policy, [&](Connection& c) {
            return c.execute(sql, args...);
        });
    }
    Result<std::int64_t> execute(const std::string& sql, const params& named) {
        return run_with_policy(default_policy(sql), [&](Connection& c) {
            return c.execute(sql, named);
        });
    }

    // --- query (returns a lease-owning QueryResult) ---
    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<QueryResult> query(const std::string& sql, const Args&... args) {
        return query_impl(default_policy(sql), [&](Connection& c) {
            return c.query(sql, args...);
        });
    }
    Result<QueryResult> query(const std::string& sql, const params& named) {
        return query_impl(default_policy(sql), [&](Connection& c) {
            return c.query(sql, named);
        });
    }

    // --- queryAs (materialized; no lease lifetime concern) ---
    template <class T, class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::vector<T>> queryAs(const std::string& sql, const Args&... args) {
        return run_with_policy(default_policy(sql), [&](Connection& c) {
            return c.template queryAs<T>(sql, args...);
        });
    }
    template <class T>
    Result<std::vector<T>> queryAs(const std::string& sql, const params& named) {
        return run_with_policy(default_policy(sql), [&](Connection& c) {
            return c.template queryAs<T>(sql, named);
        });
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
        : pool_(std::move(pool)) {
        default_attempts_ = pool_->backoff_policy().maxAttempts;
        if (default_attempts_ < 1) default_attempts_ = 1;
    }

    // Default per-call policy: read-only statements are safely auto-retried;
    // writes run once (callers opt in via the ExecPolicy execute overload).
    ExecPolicy default_policy(const std::string& sql) const {
        ExecPolicy p = detail::is_read_only(sql)
                           ? ExecPolicy::idempotent(default_attempts_)
                           : ExecPolicy::once();
        p.backoff = pool_->backoff_policy();
        return p;
    }

    // Runs op(Connection&) under a retry policy, acquiring a fresh lease each
    // attempt. A connection-class error marks the lease broken (the pool
    // discards it) so the retry connects anew.
    template <class Op>
    auto run_with_policy(const ExecPolicy& policy, Op&& op)
        -> std::invoke_result_t<Op, Connection&> {
        using R = std::invoke_result_t<Op, Connection&>;
        Error last;
        last.code = ErrorCode::Unknown;
        last.message = "no attempt made";
        for (int attempt = 1; attempt <= policy.maxAttempts; ++attempt) {
            auto lease = pool_->acquire();
            if (!lease.ok()) return R(lease.error());
            R r = op(*lease.value());
            if (r.ok()) return r;
            last = r.error();
            if (last.code == ErrorCode::Connection) lease.value().markBroken();
            if (!last.retriable || attempt == policy.maxAttempts) return r;
            policy.backoff.sleep(policy.backoff.delay_for(attempt));
        }
        return R(last);
    }

    // Like run_with_policy but preserves the successful lease inside the
    // returned QueryResult (its cursor borrows the leased connection).
    template <class Op>
    Result<QueryResult> query_impl(const ExecPolicy& policy, Op&& op) {
        Error last;
        last.code = ErrorCode::Unknown;
        last.message = "no attempt made";
        for (int attempt = 1; attempt <= policy.maxAttempts; ++attempt) {
            auto lease = pool_->acquire();
            if (!lease.ok()) return lease.error();
            auto rs = op(*lease.value());
            if (rs.ok())
                return QueryResult(std::move(lease.value()),
                                   std::move(rs.value()));
            last = rs.error();
            if (last.code == ErrorCode::Connection) lease.value().markBroken();
            if (!last.retriable || attempt == policy.maxAttempts) break;
            policy.backoff.sleep(policy.backoff.delay_for(attempt));
        }
        return last;
    }

    std::shared_ptr<ConnectionPool> pool_;
    int default_attempts_ = 3;
};

}  // namespace halcyon
