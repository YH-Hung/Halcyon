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

// `Batch` is defined in parameters.hpp so the executeBatch(Batch) overload is
// available on both Transaction and ScopedTransaction. The batchOf helpers
// below construct it.

/// \brief Build a `Batch` from a vector of `HALCYON_REFLECT`'d structs.
///
/// Each struct field (in declaration order) becomes one positional bind column.
/// Pass the result to `Connection::executeBatch`, `Transaction::executeBatch`,
/// or `Database::executeBatch`.
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

// batchOf for a vector of explicit tuple rows, e.g. rows built in a loop. More
// specialized than the batchOf(std::vector<T>) struct overload, so tuple vectors
// resolve here (no HALCYON_REFLECT required).
template <class... Cols>
Batch batchOf(const std::vector<std::tuple<Cols...>>& rows) {
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
    ~QueryResult() {
        // A mid-stream fetch failure of connection class means the leased
        // connection is dead — discard it instead of returning it to the pool.
        // No-op on a moved-from instance (lease_ released) or a clean/
        // non-connection result.
        if (lease_.get()) {
            const auto& e = rs_.error();
            if (e && e->code == ErrorCode::Connection) lease_.markBroken();
        }
    }

    ResultSet& rows() noexcept { return rs_; }
    ResultSet::iterator begin() { return rs_.begin(); }
    ResultSet::iterator end() { return rs_.end(); }
    std::size_t column_count() const noexcept { return rs_.column_count(); }

    // True unless iteration ended early on a fetch error; error() carries it.
    bool ok() const noexcept { return rs_.ok(); }
    const std::optional<Error>& error() const noexcept { return rs_.error(); }

private:
    friend class Database;
    QueryResult(std::shared_ptr<detail::cli::ICliDriver> driver,
                std::shared_ptr<ConnectionPool> pool, PooledConnection lease,
                ResultSet rs)
        : owned_driver_(std::move(driver)),
          pool_(std::move(pool)),
          lease_(std::move(lease)),
          rs_(std::move(rs)) {}

    // Keep the backing alive for this result's whole lifetime: lease_ holds a raw
    // ConnectionPool* and rs_'s cursor borrows the leased connection, so the pool
    // (and, for a dsn-opened Database, the driver it owns) must outlive them even
    // if every Database handle is destroyed first. Declaration order fixes the
    // reverse destruction order: rs_ (closes the cursor) → lease_ (returns the
    // connection) → pool_ (may tear the pool down) → owned_driver_ (last, so the
    // pool's disconnects still see a live driver). owned_driver_ is null when the
    // driver is caller-owned (injectable open).
    std::shared_ptr<detail::cli::ICliDriver> owned_driver_;
    std::shared_ptr<ConnectionPool> pool_;
    PooledConnection lease_;
    ResultSet rs_;
};

/// \brief A Transaction plus the pooled lease it runs on, returned by `Database::begin()`.
///
/// Move-only; `operator->` and `operator*` forward to the inner Transaction.
/// Rolls back automatically on destruction if not committed. Returns the
/// connection to the pool when destroyed.
class ScopedTransaction {
public:
    ScopedTransaction(ScopedTransaction&&) noexcept = default;
    ScopedTransaction& operator=(ScopedTransaction&&) noexcept = default;
    ScopedTransaction(const ScopedTransaction&) = delete;
    ScopedTransaction& operator=(const ScopedTransaction&) = delete;
    ~ScopedTransaction() {
        // Drive any implicit rollback here (rather than leaving it to txn_'s
        // destructor) so we can observe its outcome before lease_ is released: if
        // the transaction left the connection dead or with autocommit in an
        // unknown state, discard it instead of returning it to the pool. No-op on
        // a moved-from instance (lease_ released).
        if (lease_.get()) {
            if (txn_.active()) txn_.rollback();
            if (txn_.poisoned()) lease_.markBroken();
        }
    }

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
    Result<std::int64_t> executeBatch(const std::string& sql,
                                      const Batch& batch) {
        return txn_.executeBatch(sql, batch.rows);
    }
    Result<void> commit() { return txn_.commit(); }
    Result<void> rollback() { return txn_.rollback(); }

private:
    friend class Database;
    ScopedTransaction(std::shared_ptr<detail::cli::ICliDriver> driver,
                      std::shared_ptr<ConnectionPool> pool,
                      PooledConnection lease, Transaction txn)
        : owned_driver_(std::move(driver)),
          pool_(std::move(pool)),
          lease_(std::move(lease)),
          txn_(std::move(txn)) {}

    // Keep the backing alive for the unit of work even if every Database handle
    // is destroyed first (see QueryResult). Declaration order fixes the reverse
    // destruction order: txn_ (rolls back if uncommitted) → lease_ (returns the
    // connection) → pool_ → owned_driver_ (last).
    std::shared_ptr<detail::cli::ICliDriver> owned_driver_;
    std::shared_ptr<ConnectionPool> pool_;
    PooledConnection lease_;  // destroyed AFTER txn_
    Transaction txn_;
};

/// \brief High-level entry point: a pooled, thread-safe Db2 database handle.
///
/// Open with `Database::open` (returns `Result<Database>`) or
/// `Database::openOrThrow`. Provides query/execute/transaction/batch in both
/// `Result<T>` and throwing styles. Safe to share across threads.
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

    // Injectable entry point: opens over a caller-owned driver. The driver is NOT
    // co-owned, so the caller must keep it alive not merely for the Database's
    // lifetime but for the lifetime of EVERY object derived from it — QueryResult,
    // ScopedTransaction, and async futures can each outlive the Database and still
    // use the driver. Destroying the driver while any of those are live is a
    // use-after-free. For automatic lifetime safety, use the shared_ptr overload
    // below. Used by unit tests to drive the facade against a MockCliDriver.
    static Result<Database> open(detail::cli::ICliDriver& driver,
                                 const std::string& dsn, PoolConfig config = {}) {
        return open_impl(nullptr, driver, dsn, std::move(config));
    }
    static Database openOrThrow(detail::cli::ICliDriver& driver,
                                const std::string& dsn, PoolConfig config = {}) {
        return open(driver, dsn, std::move(config)).value();
    }

    // Shared-driver entry point: the Database co-owns the driver, so it stays
    // alive as long as the Database OR any object/future derived from it (just
    // like the driver the dsn-only open creates internally). Prefer this over the
    // raw-reference overload whenever cursors/transactions may outlive the
    // Database.
    static Result<Database> open(std::shared_ptr<detail::cli::ICliDriver> driver,
                                 const std::string& dsn, PoolConfig config = {}) {
        if (!driver) {
            Error e;
            e.code = ErrorCode::Connection;
            e.message = "null driver passed to Database::open";
            return e;
        }
        auto& ref = *driver;
        return open_impl(std::move(driver), ref, dsn, std::move(config));
    }
    static Database openOrThrow(std::shared_ptr<detail::cli::ICliDriver> driver,
                                const std::string& dsn, PoolConfig config = {}) {
        return open(std::move(driver), dsn, std::move(config)).value();
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
    // Streaming execute (LobSource args). Always a single attempt: a LOB
    // source's stream is consumed by the first try and cannot be replayed.
    template <class... Args,
              std::enable_if_t<detail::stream_pack_ok<Args...>::value, int> = 0>
    Result<std::int64_t> execute(const std::string& sql, const Args&... args) {
        return instrument(metrics_, tracer_, has_metrics_, has_tracer_,
                          "halcyon.execute", sql, [&] {
                              return run_with_policy(ExecPolicy::once(),
                                                     [&](Connection& c) {
                                                         return c.execute(sql,
                                                                          args...);
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
        auto r = lease.value()->executeBatch(sql, batch.rows);
        // executeBatch has no auto-retry, so a dead connection would otherwise be
        // returned to the pool — discard it here.
        if (!r.ok() && r.error().code == ErrorCode::Connection)
            lease.value().markBroken();
        return r;
    }

    // --- transactions ---

    // Begins a transaction on a freshly leased connection; the returned
    // ScopedTransaction owns the lease for the unit of work's lifetime.
    Result<ScopedTransaction> begin() {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        auto tx = lease.value()->begin();
        if (!tx.ok()) {
            // begin() flips autocommit off; a failure can leave the connection
            // dead or with autocommit in an unknown state, so discard it rather
            // than returning it to the pool.
            lease.value().markBroken();
            return tx.error();
        }
        return ScopedTransaction(owned_driver_, pool_, std::move(lease.value()),
                                 std::move(tx.value()));
    }

    // Begins a transaction at `level` on a freshly leased connection; the
    // connection's default isolation is restored when the unit of work ends.
    Result<ScopedTransaction> begin(Isolation level) {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        auto tx = lease.value()->begin(level);
        if (!tx.ok()) {
            lease.value().markBroken();
            return tx.error();
        }
        return ScopedTransaction(owned_driver_, pool_, std::move(lease.value()),
                                 std::move(tx.value()));
    }

    // Functional transaction: commit on a successful Result, rollback on an
    // error Result or a thrown exception (then rethrow). fn must return
    // Result<U>.
    template <class Fn>
    auto transaction(Fn&& fn) -> std::invoke_result_t<Fn, Transaction&> {
        return run_transaction([this] { return begin(); },
                               std::forward<Fn>(fn));
    }
    // As above, but runs the unit of work at `level`.
    template <class Fn>
    auto transaction(Isolation level, Fn&& fn)
        -> std::invoke_result_t<Fn, Transaction&> {
        return run_transaction([this, level] { return begin(level); },
                               std::forward<Fn>(fn));
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
        auto parent = has_tracer_ ? tracer_->captureContext()
                                  : std::shared_ptr<obs::SpanContext>{};
        return exec_->submit(
            [pool = pool_, attempts = default_attempts_,
             parent = std::move(parent), sql, args...]() {
                return instrument(
                    pool->metrics(), pool->tracer(), pool->metrics_enabled(),
                    pool->tracer_enabled(), "halcyon.execute", sql,
                    [&] {
                        return run_with_policy_on(
                            *pool, default_policy_for(*pool, attempts, sql),
                            [&](Connection& c) { return c.execute(sql, args...); },
                            pool->metrics(), pool->metrics_enabled());
                    },
                    parent.get());
            });
    }
    // Typed async query (spec §5): materializes rows into std::vector<T> so the
    // future carries no pool-lease lifetime. (Streaming async over a live cursor
    // is a documented non-goal/future extension.)
    template <class T, class... Args>
    std::future<Result<std::vector<T>>> queryAsync(const std::string& sql,
                                                   Args... args) {
        auto parent = has_tracer_ ? tracer_->captureContext()
                                  : std::shared_ptr<obs::SpanContext>{};
        return exec_->submit(
            [pool = pool_, attempts = default_attempts_,
             parent = std::move(parent), sql, args...]() {
                return instrument(
                    pool->metrics(), pool->tracer(), pool->metrics_enabled(),
                    pool->tracer_enabled(), "halcyon.query", sql,
                    [&] {
                        return run_with_policy_on(
                            *pool, default_policy_for(*pool, attempts, sql),
                            [&](Connection& c) {
                                return c.template queryAs<T>(sql, args...);
                            },
                            pool->metrics(), pool->metrics_enabled());
                    },
                    parent.get());
            });
    }

    // Parent all Halcyon spans created on the current thread under `ctx` until
    // the returned guard is destroyed — synchronous calls and any async work
    // submitted while it is held. `ctx` comes from the OTel adapter
    // (obs::make_otel_active_context / obs::extract_otel_context). No-op (empty
    // guard) when no tracer is configured.
    [[nodiscard]] obs::ScopedContext useParentContext(
        std::shared_ptr<obs::SpanContext> ctx) {
        return obs::ScopedContext(
            has_tracer_ ? tracer_->attachContext(std::move(ctx))
                        : std::unique_ptr<obs::ContextToken>{});
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
        return query_impl_on(pool_, owned_driver_, policy,
                             std::forward<Op>(op), metrics_, has_metrics_);
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
                           const std::string& sql, Fn&& fn,
                           const obs::SpanContext* parent = nullptr)
        -> std::invoke_result_t<Fn> {
        if (!hasM && !hasT) return fn();  // zero-overhead default

        const std::string_view op = detail::obs::op_label(sql);
        obs::ScopedSpan span =
            hasT ? obs::ScopedSpan(t->startSpan(
                       spanName,
                       {{"db.system", "db2"}, {"db.statement", sql}}, parent))
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
    // Shared functional-transaction body: opens the transaction span, begins the
    // unit of work through `beginFn` (so the acquire span nests under the txn
    // span), then wraps `fn` (commit on ok, rollback on error/exception). Keeps
    // transaction(fn) and transaction(iso, fn) DRY. `beginFn` returns
    // Result<ScopedTransaction>.
    template <class Begin, class Fn>
    auto run_transaction(Begin&& beginFn, Fn&& fn)
        -> std::invoke_result_t<Fn, Transaction&> {
        using R = std::invoke_result_t<Fn, Transaction&>;
        obs::ScopedSpan span =
            has_tracer_ ? obs::ScopedSpan(tracer_->startSpan(
                              "halcyon.transaction", {{"db.system", "db2"}}))
                        : obs::ScopedSpan();
        auto st = beginFn();  // acquire nests under the active transaction span
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

    static void emit_retry(ConnectionPool& pool, obs::MetricsSink* m, bool hasM,
                           const char* outcome) {
        if (hasM) m->counter("halcyon_retries_total", 1.0, {{"outcome", outcome}});
        if (pool.logger() != nullptr)
            pool.logger()->log(obs::LogLevel::Warn, "retry.attempt",
                               {{"outcome", outcome}});
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
                    emit_retry(pool, m, hasM, "exhausted");
                return r;
            }
            emit_retry(pool, m, hasM, "retried");
            policy.backoff.sleep(policy.backoff.delay_for(attempt));
        }
        return R(last);
    }

    // Like run_with_policy_on but preserves the successful lease inside the
    // returned QueryResult (its cursor borrows the leased connection). Takes the
    // pool (and owned driver) as shared_ptrs so the returned QueryResult can keep
    // them alive past the destruction of every Database handle (see QueryResult).
    template <class Op>
    static Result<QueryResult> query_impl_on(
        const std::shared_ptr<ConnectionPool>& pool,
        const std::shared_ptr<detail::cli::ICliDriver>& driver,
        const ExecPolicy& policy, Op&& op, obs::MetricsSink* m = nullptr,
        bool hasM = false) {
        Error last;
        last.code = ErrorCode::Unknown;
        last.message = "no attempt made";
        for (int attempt = 1; attempt <= policy.maxAttempts; ++attempt) {
            auto lease = pool->acquire();
            if (!lease.ok()) return lease.error();
            auto rs = op(*lease.value());
            if (rs.ok())
                return QueryResult(driver, pool, std::move(lease.value()),
                                   std::move(rs.value()));
            last = rs.error();
            if (last.code == ErrorCode::Connection) lease.value().markBroken();
            if (!last.retriable || attempt == policy.maxAttempts) {
                if (attempt > 1 && attempt == policy.maxAttempts)
                    emit_retry(*pool, m, hasM, "exhausted");
                break;
            }
            emit_retry(*pool, m, hasM, "retried");
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
template <class Fn>
auto transaction(Database& db, Isolation level, Fn&& fn) {
    return db.transaction(level, std::forward<Fn>(fn));
}

}  // namespace halcyon
