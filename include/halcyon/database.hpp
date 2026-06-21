#pragma once

#include <cstddef>
#include <cstdint>
#include <future>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "halcyon/async.hpp"
#include "halcyon/connection.hpp"
#include "halcyon/detail/cli/db2_cli_driver.hpp"
#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/error.hpp"
#include "halcyon/observability/instrument.hpp"
#include "halcyon/observability/metrics.hpp"
#include "halcyon/observability/tracing.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/pool.hpp"
#include "halcyon/result.hpp"
#include "halcyon/retry.hpp"
#include "halcyon/transaction.hpp"
#include "halcyon/types.hpp"

namespace halcyon {

namespace detail {

// Appends each reflected field of item (in declaration order) as a bound Value.
template <class T, class Tuple, std::size_t... I>
void batch_append_fields(std::vector<detail::cli::Value>& row, const T& item,
                         const Tuple& mptrs, std::index_sequence<I...>) {
    (row.push_back(detail::to_value(item.*std::get<I>(mptrs))), ...);
}

// Appends each element of a tuple row (in order) as a bound Value.
template <class Tuple, std::size_t... I>
void batch_append_tuple(std::vector<detail::cli::Value>& row, const Tuple& t,
                        std::index_sequence<I...>) {
    (row.push_back(detail::to_value(std::get<I>(t))), ...);
}

}  // namespace detail

// A prepared set of positional parameter rows for executeBatch.
struct Batch {
    std::vector<std::vector<detail::cli::Value>> rows;
};

// batchOf for a vector of HALCYON_REFLECT'd structs: each field becomes a
// positional bind in declaration order.
template <class T>
Batch batchOf(const std::vector<T>& items) {
    static_assert(reflect::Reflected<T>::value,
                  "batchOf<T> requires HALCYON_REFLECT(T, fields...)");
    Batch out;
    out.rows.reserve(items.size());
    auto mptrs = reflect::Reflected<T>::members();
    constexpr std::size_t N = reflect::Reflected<T>::field_count;
    for (const auto& item : items) {
        std::vector<detail::cli::Value> row;
        row.reserve(N);
        detail::batch_append_fields(row, item, mptrs,
                                    std::make_index_sequence<N>{});
        out.rows.push_back(std::move(row));
    }
    return out;
}

// batchOf for an initializer list of reflected structs: batchOf<T>({{...},{...}}).
template <class T>
Batch batchOf(std::initializer_list<T> items) {
    return batchOf(std::vector<T>(items));
}

// batchOf for explicit tuple rows: batchOf({std::make_tuple(1, "a"), ...}).
// Each tuple element becomes a positional bind in order — no HALCYON_REFLECT
// required. More specialized than the initializer_list<T> overload above, so it
// is preferred for tuple rows.
template <class... Cols>
Batch batchOf(std::initializer_list<std::tuple<Cols...>> rows) {
    Batch out;
    out.rows.reserve(rows.size());
    for (const auto& t : rows) {
        std::vector<detail::cli::Value> row;
        row.reserve(sizeof...(Cols));
        detail::batch_append_tuple(row, t, std::index_sequence_for<Cols...>{});
        out.rows.push_back(std::move(row));
    }
    return out;
}

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

    // True unless iteration ended early on a fetch error; error() carries it.
    bool ok() const noexcept { return rs_.ok(); }
    const std::optional<Error>& error() const noexcept { return rs_.error(); }

private:
    friend class Database;
    QueryResult(PooledConnection lease, ResultSet rs)
        : lease_(std::move(lease)), rs_(std::move(rs)) {}

    PooledConnection lease_;  // declared first → destroyed last (after rs_)
    ResultSet rs_;
};

// A Transaction plus the pooled lease it runs on, so both are released together.
// Returned by Database::begin(); move-only. operator-> forwards to the
// Transaction. Declaration order matters: txn_ is destroyed (rolls back if not
// committed) BEFORE lease_ returns the connection to the pool.
class ScopedTransaction {
public:
    ScopedTransaction(ScopedTransaction&&) noexcept = default;
    ScopedTransaction& operator=(ScopedTransaction&&) noexcept = default;
    ScopedTransaction(const ScopedTransaction&) = delete;
    ScopedTransaction& operator=(const ScopedTransaction&) = delete;

    Transaction* operator->() noexcept { return &txn_; }
    Transaction& operator*() noexcept { return txn_; }

    bool active() const noexcept { return txn_.active(); }
    template <class... Args>
    Result<std::int64_t> execute(const std::string& sql, const Args&... a) {
        return txn_.execute(sql, a...);
    }
    Result<std::int64_t> execute(const std::string& sql, const params& n) {
        return txn_.execute(sql, n);
    }
    template <class... Args>
    Result<ResultSet> query(const std::string& sql, const Args&... a) {
        return txn_.query(sql, a...);
    }
    Result<ResultSet> query(const std::string& sql, const params& n) {
        return txn_.query(sql, n);
    }
    template <class T, class... Args>
    Result<std::vector<T>> queryAs(const std::string& sql, const Args&... a) {
        return txn_.template queryAs<T>(sql, a...);
    }
    Result<void> commit() { return txn_.commit(); }
    Result<void> rollback() { return txn_.rollback(); }

private:
    friend class Database;
    ScopedTransaction(PooledConnection lease, Transaction txn)
        : lease_(std::move(lease)), txn_(std::move(txn)) {}

    PooledConnection lease_;  // destroyed AFTER txn_
    Transaction txn_;
};

// High-level thread-safe entry point. Copyable handle: copies share one pool
// (shared_ptr), so a Database can be passed by value across threads. Each call
// acquires a pooled connection, runs, and releases it on return.
class Database {
public:
    // Ergonomic entry point (spec §5): opens over a real IBM Db2 CLI driver that
    // the Database creates and owns for its lifetime. No CLI types in the call.
    static Result<Database> open(const std::string& dsn, PoolConfig config = {}) {
        auto drv = detail::cli::make_db2_cli_driver();
        auto& ref = *drv;
        return open_impl(std::move(drv), ref, dsn, std::move(config));
    }
    static Database openOrThrow(const std::string& dsn, PoolConfig config = {}) {
        return open(dsn, std::move(config)).value();
    }

    // Injectable entry point: opens over a caller-owned driver (the caller must
    // keep it alive for the Database's lifetime). Used by unit tests to drive the
    // facade against a MockCliDriver with no live Db2.
    static Result<Database> open(detail::cli::ICliDriver& driver,
                                 const std::string& dsn, PoolConfig config = {}) {
        return open_impl(nullptr, driver, dsn, std::move(config));
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
        return instrument(metrics_, tracer_, has_metrics_, has_tracer_,
                          "halcyon.execute", sql, [&] {
            return run_with_policy(default_policy(sql), [&](Connection& c) {
                return c.execute(sql, args...);
            });
        });
    }
    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::int64_t> execute(const std::string& sql, const ExecPolicy& policy,
                                 const Args&... args) {
        return instrument(metrics_, tracer_, has_metrics_, has_tracer_,
                          "halcyon.execute", sql, [&] {
            return run_with_policy(policy, [&](Connection& c) {
                return c.execute(sql, args...);
            });
        });
    }
    Result<std::int64_t> execute(const std::string& sql, const params& named) {
        return instrument(metrics_, tracer_, has_metrics_, has_tracer_,
                          "halcyon.execute", sql, [&] {
            return run_with_policy(default_policy(sql), [&](Connection& c) {
                return c.execute(sql, named);
            });
        });
    }

    // --- query (returns a lease-owning QueryResult) ---
    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<QueryResult> query(const std::string& sql, const Args&... args) {
        return instrument(metrics_, tracer_, has_metrics_, has_tracer_,
                          "halcyon.query", sql, [&] {
            return query_impl(default_policy(sql), [&](Connection& c) {
                return c.query(sql, args...);
            });
        });
    }
    Result<QueryResult> query(const std::string& sql, const params& named) {
        return instrument(metrics_, tracer_, has_metrics_, has_tracer_,
                          "halcyon.query", sql, [&] {
            return query_impl(default_policy(sql), [&](Connection& c) {
                return c.query(sql, named);
            });
        });
    }

    // --- queryAs (materialized; no lease lifetime concern) ---
    template <class T, class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::vector<T>> queryAs(const std::string& sql, const Args&... args) {
        return instrument(metrics_, tracer_, has_metrics_, has_tracer_,
                          "halcyon.query", sql, [&] {
            return run_with_policy(default_policy(sql), [&](Connection& c) {
                return c.template queryAs<T>(sql, args...);
            });
        });
    }
    template <class T>
    Result<std::vector<T>> queryAs(const std::string& sql, const params& named) {
        return instrument(metrics_, tracer_, has_metrics_, has_tracer_,
                          "halcyon.query", sql, [&] {
            return run_with_policy(default_policy(sql), [&](Connection& c) {
                return c.template queryAs<T>(sql, named);
            });
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

    // --- batch (writes; no auto-retry — caller re-drives on failure) ---
    Result<std::int64_t> executeBatch(const std::string& sql,
                                      const Batch& batch) {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        return lease.value()->executeBatch(sql, batch.rows);
    }

    // --- transactions ---

    // Begins a transaction on a freshly leased connection; the returned
    // ScopedTransaction owns the lease for the unit of work's lifetime.
    Result<ScopedTransaction> begin() {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        auto tx = lease.value()->begin();
        if (!tx.ok()) return tx.error();
        return ScopedTransaction(std::move(lease.value()),
                                 std::move(tx.value()));
    }

    // Functional transaction: commit on a successful Result, rollback on an
    // error Result or a thrown exception (then rethrow). fn must return
    // Result<U>.
    template <class Fn>
    auto transaction(Fn&& fn) -> std::invoke_result_t<Fn, Transaction&> {
        using R = std::invoke_result_t<Fn, Transaction&>;
        obs::ScopedSpan span =
            has_tracer_ ? obs::ScopedSpan(tracer_->startSpan(
                              "halcyon.transaction", {{"db.system", "db2"}}))
                        : obs::ScopedSpan();
        auto st = begin();
        if (!st.ok()) {
            span.setStatusError(st.error().sqlstate);
            return R(st.error());
        }
        R r = [&]() -> R {
            try {
                return std::forward<Fn>(fn)(*st.value());
            } catch (...) {
                st.value().rollback();
                throw;
            }
        }();
        if (r.ok()) {
            auto c = st.value().commit();
            if (!c.ok()) {
                span.setStatusError(c.error().sqlstate);
                return R(c.error());
            }
        } else {
            span.setStatusError(r.error().sqlstate);
            st.value().rollback();
        }
        return r;
    }

    // --- async (std::future, backed by a shared Executor) ---
    // The task captures the shared pool (a shared_ptr that keeps the backing
    // alive), never `this`: a Database is a copyable handle, so the specific
    // copy that launched the task may be destroyed while another copy keeps the
    // shared executor running. The shared Executor (held by every copy) drains
    // in-flight tasks on the last copy's destruction, so the captured pool stays
    // valid for the task's whole lifetime.
    template <class... Args>
    std::future<Result<std::int64_t>> executeAsync(const std::string& sql,
                                                    Args... args) {
        return exec_->submit(
            [pool = pool_, attempts = default_attempts_, sql, args...]() {
                return instrument(
                    pool->metrics(), pool->tracer(), pool->metrics_enabled(),
                    pool->tracer_enabled(), "halcyon.execute", sql, [&] {
                        return run_with_policy_on(
                            *pool, default_policy_for(*pool, attempts, sql),
                            [&](Connection& c) { return c.execute(sql, args...); },
                            pool->metrics(), pool->metrics_enabled());
                    });
            });
    }
    // Typed async query (spec §5): materializes rows into std::vector<T> so the
    // future carries no pool-lease lifetime. (Streaming async over a live cursor
    // is a documented non-goal/future extension.)
    template <class T, class... Args>
    std::future<Result<std::vector<T>>> queryAsync(const std::string& sql,
                                                   Args... args) {
        return exec_->submit(
            [pool = pool_, attempts = default_attempts_, sql, args...]() {
                return instrument(
                    pool->metrics(), pool->tracer(), pool->metrics_enabled(),
                    pool->tracer_enabled(), "halcyon.query", sql, [&] {
                        return run_with_policy_on(
                            *pool, default_policy_for(*pool, attempts, sql),
                            [&](Connection& c) {
                                return c.template queryAs<T>(sql, args...);
                            },
                            pool->metrics(), pool->metrics_enabled());
                    });
            });
    }

private:
    Database(std::shared_ptr<detail::cli::ICliDriver> owned_driver,
             std::shared_ptr<ConnectionPool> pool, std::shared_ptr<Executor> exec)
        : owned_driver_(std::move(owned_driver)),
          pool_(std::move(pool)),
          exec_(std::move(exec)) {
        default_attempts_ = pool_->backoff_policy().maxAttempts;
        if (default_attempts_ < 1) default_attempts_ = 1;
        // Cache the pool's resolved sinks for the synchronous hot path; async
        // tasks read them from the captured pool instead (no `this`).
        metrics_ = pool_->metrics();
        tracer_ = pool_->tracer();
        has_metrics_ = pool_->metrics_enabled();
        has_tracer_ = pool_->tracer_enabled();
    }

    // Builds a Database over driver, optionally taking ownership of it (owned is
    // non-null only for the dsn-only open that created the driver itself).
    static Result<Database> open_impl(
        std::shared_ptr<detail::cli::ICliDriver> owned,
        detail::cli::ICliDriver& driver, const std::string& dsn,
        PoolConfig config) {
        const std::size_t threads = config.max ? config.max : 1;
        auto pool = ConnectionPool::create(driver, {dsn}, std::move(config));
        if (!pool.ok()) return pool.error();
        return Database(std::move(owned),
                        std::shared_ptr<ConnectionPool>(std::move(pool.value())),
                        std::make_shared<Executor>(threads));
    }

    // Default per-call policy: read-only statements are safely auto-retried;
    // writes run once (callers opt in via the ExecPolicy execute overload).
    ExecPolicy default_policy(const std::string& sql) const {
        return default_policy_for(*pool_, default_attempts_, sql);
    }
    template <class Op>
    auto run_with_policy(const ExecPolicy& policy, Op&& op)
        -> std::invoke_result_t<Op, Connection&> {
        return run_with_policy_on(*pool_, policy, std::forward<Op>(op), metrics_,
                                  has_metrics_);
    }
    template <class Op>
    Result<QueryResult> query_impl(const ExecPolicy& policy, Op&& op) {
        return query_impl_on(*pool_, policy, std::forward<Op>(op), metrics_,
                             has_metrics_);
    }

    // Wraps one logical query/execute with the terminal metrics
    // (halcyon_queries_total, halcyon_query_duration_seconds, halcyon_errors_total)
    // and a span (halcyon.query / halcyon.execute). Static and `this`-free so the
    // async path can call it with the captured pool's sinks. retries_total is
    // emitted separately inside the run helpers. The uninstrumented path is a
    // single branch with no allocation. fn() must return a Result<…>.
    template <class Fn>
    static auto instrument(obs::MetricsSink* m, obs::Tracer* t, bool hasM,
                           bool hasT, std::string_view spanName,
                           const std::string& sql, Fn&& fn)
        -> std::invoke_result_t<Fn> {
        if (!hasM && !hasT) return fn();  // zero-overhead default

        const std::string_view op = detail::obs::op_label(sql);
        obs::ScopedSpan span =
            hasT ? obs::ScopedSpan(t->startSpan(
                       spanName, {{"db.system", "db2"}, {"db.statement", sql}}))
                 : obs::ScopedSpan();
        detail::obs::Timer timer;
        auto r = fn();
        if (hasM) {
            m->histogram("halcyon_query_duration_seconds",
                         timer.elapsed_seconds(), {{"op", op}});
            m->counter("halcyon_queries_total", 1.0,
                       {{"op", op}, {"status", r.ok() ? "ok" : "error"}});
            if (!r.ok())
                m->counter("halcyon_errors_total", 1.0,
                           {{"code", detail::obs::code_label(r.error().code)}});
        }
        if (hasT) {
            if (!r.ok()) {
                span.setStatusError(r.error().sqlstate);
            } else if constexpr (std::is_same_v<
                                     std::decay_t<decltype(r.value())>,
                                     std::int64_t>) {
                // execute()/affected-row paths carry a count (spec §9 attr).
                span.setAttribute("db.rows_affected", std::to_string(r.value()));
            }
        }
        return r;
    }

    // --- static run logic (no `this`) ---
    // These take the shared pool explicitly so async tasks can drive a query by
    // capturing only the pool (a shared_ptr that keeps the backing alive),
    // never a Database instance. Capturing `this` would dangle the moment the
    // launching Database copy is destroyed while another copy keeps the shared
    // executor running.
    static ExecPolicy default_policy_for(ConnectionPool& pool, int attempts,
                                         const std::string& sql) {
        ExecPolicy p = detail::is_read_only(sql) ? ExecPolicy::idempotent(attempts)
                                                 : ExecPolicy::once();
        p.backoff = pool.backoff_policy();
        return p;
    }

    // Runs op(Connection&) under a retry policy, acquiring a fresh lease each
    // attempt. A connection-class error marks the lease broken (the pool
    // discards it) so the retry connects anew.
    // Emits halcyon_retries_total: {outcome=retried} before each replay,
    // {outcome=exhausted} when a retried op finally fails out of attempts.
    static void emit_retry(obs::MetricsSink* m, bool hasM, const char* outcome) {
        if (hasM) m->counter("halcyon_retries_total", 1.0, {{"outcome", outcome}});
    }

    template <class Op>
    static auto run_with_policy_on(ConnectionPool& pool, const ExecPolicy& policy,
                                   Op&& op, obs::MetricsSink* m = nullptr,
                                   bool hasM = false)
        -> std::invoke_result_t<Op, Connection&> {
        using R = std::invoke_result_t<Op, Connection&>;
        Error last;
        last.code = ErrorCode::Unknown;
        last.message = "no attempt made";
        for (int attempt = 1; attempt <= policy.maxAttempts; ++attempt) {
            auto lease = pool.acquire();
            if (!lease.ok()) return R(lease.error());
            R r = op(*lease.value());
            if (r.ok()) return r;
            last = r.error();
            if (last.code == ErrorCode::Connection) lease.value().markBroken();
            if (!last.retriable || attempt == policy.maxAttempts) {
                if (attempt > 1 && attempt == policy.maxAttempts)
                    emit_retry(m, hasM, "exhausted");
                return r;
            }
            emit_retry(m, hasM, "retried");
            policy.backoff.sleep(policy.backoff.delay_for(attempt));
        }
        return R(last);
    }

    // Like run_with_policy_on but preserves the successful lease inside the
    // returned QueryResult (its cursor borrows the leased connection).
    template <class Op>
    static Result<QueryResult> query_impl_on(ConnectionPool& pool,
                                             const ExecPolicy& policy, Op&& op,
                                             obs::MetricsSink* m = nullptr,
                                             bool hasM = false) {
        Error last;
        last.code = ErrorCode::Unknown;
        last.message = "no attempt made";
        for (int attempt = 1; attempt <= policy.maxAttempts; ++attempt) {
            auto lease = pool.acquire();
            if (!lease.ok()) return lease.error();
            auto rs = op(*lease.value());
            if (rs.ok())
                return QueryResult(std::move(lease.value()),
                                   std::move(rs.value()));
            last = rs.error();
            if (last.code == ErrorCode::Connection) lease.value().markBroken();
            if (!last.retriable || attempt == policy.maxAttempts) {
                if (attempt > 1 && attempt == policy.maxAttempts)
                    emit_retry(m, hasM, "exhausted");
                break;
            }
            emit_retry(m, hasM, "retried");
            policy.backoff.sleep(policy.backoff.delay_for(attempt));
        }
        return last;
    }

    // Non-null only when this Database created the driver (dsn-only open).
    // Declared before pool_ so it is destroyed AFTER the pool, which holds a
    // bare reference to the driver.
    std::shared_ptr<detail::cli::ICliDriver> owned_driver_;
    std::shared_ptr<ConnectionPool> pool_;
    std::shared_ptr<Executor> exec_;
    int default_attempts_ = 3;

    // Observability sinks cached from the pool (never null; flags gate emission).
    obs::MetricsSink* metrics_ = nullptr;
    obs::Tracer* tracer_ = nullptr;
    bool has_metrics_ = false;
    bool has_tracer_ = false;
};

// --- Functional free-function API (delegates to Database) ---

template <class... Args>
auto query(Database& db, const std::string& sql, const Args&... args) {
    return db.query(sql, args...);
}
inline auto query(Database& db, const std::string& sql, const params& named) {
    return db.query(sql, named);
}
template <class... Args>
auto execute(Database& db, const std::string& sql, const Args&... args) {
    return db.execute(sql, args...);
}
inline auto execute(Database& db, const std::string& sql, const params& named) {
    return db.execute(sql, named);
}
template <class T, class... Args>
auto query_as(Database& db, const std::string& sql, const Args&... args) {
    return db.template queryAs<T>(sql, args...);
}
template <class T>
auto query_as(Database& db, const std::string& sql, const params& named) {
    return db.template queryAs<T>(sql, named);
}
template <class Fn>
auto transaction(Database& db, Fn&& fn) {
    return db.transaction(std::forward<Fn>(fn));
}

}  // namespace halcyon
