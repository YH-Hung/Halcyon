#pragma once

#include "halcyon/coro/detail/require_coroutines.hpp"

#if HALCYON_HAS_CORO_SUPPORT  // swallow everything after the guard's #error

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "halcyon/coro/detail/offload.hpp"
#include "halcyon/coro/task.hpp"
#include "halcyon/database.hpp"
#include "halcyon/isolation.hpp"
#include "halcyon/parameters.hpp"

namespace halcyon::coro {

namespace detail {

// Internal coroutines: every argument arrives OWNED (bundle, sql string,
// tuple of owned Values / LobSource wrappers). The public factories below are
// NON-coroutine functions that materialize before any lazy frame exists — a
// hard structural rule (spec §A.3): a lazy coroutine taking Args&&... would
// bind dangling references into its frame and capture the trace context on
// whatever thread later awaits it.
template <class Owned>
Task<Result<std::int64_t>> execute_task(Database::AsyncBacking backing,
                                        std::string sql, Owned owned) {
    co_return co_await offload(backing, [&] {
        return std::apply(
            [&](const auto&... vs) {
                return backing.syncView.execute(sql, vs...);
            },
            owned);
    });
}

inline Task<Result<std::int64_t>> execute_named_task(
    Database::AsyncBacking backing, std::string sql, params named) {
    co_return co_await offload(
        backing, [&] { return backing.syncView.execute(sql, named); });
}

template <class T, class Owned>
Task<Result<std::vector<T>>> query_task(Database::AsyncBacking backing,
                                        std::string sql, Owned owned) {
    co_return co_await offload(backing, [&] {
        return std::apply(
            [&](const auto&... vs) {
                return backing.syncView.template queryAs<T>(sql, vs...);
            },
            owned);
    });
}

template <class T>
Task<Result<std::vector<T>>> query_named_task(Database::AsyncBacking backing,
                                              std::string sql, params named) {
    co_return co_await offload(backing, [&] {
        return backing.syncView.template queryAs<T>(sql, named);
    });
}

inline Task<Result<std::int64_t>> execute_batch_task(
    Database::AsyncBacking backing, std::string sql, Batch batch) {
    co_return co_await offload(
        backing, [&] { return backing.syncView.executeBatch(sql, batch); });
}

template <class T>
struct is_task : std::false_type {};
template <class T>
struct is_task<Task<T>> : std::true_type {};
template <class T>
struct task_payload;
template <class T>
struct task_payload<Task<T>> {
    using type = T;
};

// Plain Result-returning callable: the whole functional transaction runs in
// ONE offload on a worker — span, commit-on-ok, rollback on error/exception
// (rethrown from co_await), and poison-discard are literally the sync
// transaction's. Per-op awaitables inside fn would be pointless executor hops:
// the code is already on a worker; use blocking tx.execute/tx.query.
template <class Fn, class R>
Task<R> transaction_plain_task(Database::AsyncBacking backing,
                               std::optional<Isolation> iso, Fn fn) {
    co_return co_await offload(backing, [&] {
        return iso ? backing.syncView.transaction(*iso, fn)
                   : backing.syncView.transaction(fn);
    });
}

// Task-returning coroutine fn: begin on a worker, co_await fn (a foreign
// awaitable controls where it resumes — every tx.* call blocks whatever
// thread fn currently occupies), then HOP BACK to a Halcyon worker so
// commit/rollback always run on workers. If the hop is impossible (executor
// gone), roll back synchronously on the current thread rather than leak an
// open transaction (documented edge). The Transaction is only ever touched
// sequentially, satisfying the single-owner contract.
//
// TRACING (documented limitation): unlike the plain-callable path, this path
// does NOT emit a "halcyon.transaction" span — it bypasses the instrumented
// sync transaction() by design (begin / foreign awaits / hop-back). Context
// propagation and the acquire span still apply. Follow-up recorded for full
// span parity.
template <class Fn, class R>
Task<R> transaction_coro_task(Database::AsyncBacking backing,
                              std::optional<Isolation> iso, Fn fn) {
    auto st = co_await offload(backing, [&] {
        return iso ? backing.syncView.begin(*iso) : backing.syncView.begin();
    });
    if (!st.ok()) co_return R(st.error());
    ScopedTransaction tx = std::move(st.value());

    std::optional<R> r;
    std::exception_ptr thrown;
    try {
        r.emplace(co_await fn(*tx));
    } catch (...) {
        thrown = std::current_exception();
    }

    const bool onWorker = co_await HopToWorker(backing);
    if (!onWorker) {
        tx.rollback();  // synchronous fallback; never leak an open transaction
        if (thrown) std::rethrow_exception(thrown);
        if (!r->ok()) co_return std::move(*r);
        co_return R(executor_gone_error());
    }
    if (thrown) {
        tx.rollback();
        std::rethrow_exception(thrown);
    }
    if (!r->ok()) {
        tx.rollback();
        co_return std::move(*r);
    }
    auto c = tx.commit();
    if (!c.ok()) co_return R(c.error());
    co_return std::move(*r);
}

}  // namespace detail

/// \brief Awaitable Database::execute (positional and LOB-streaming args).
///
/// Non-coroutine factory: runs synchronously at the call site, where it
/// captures the AsyncBacking bundle (the OTel trace context is snapshotted
/// here), copies `sql`, and materializes every ordinary argument into an
/// owned seam Value — passing temporaries that die before the first co_await
/// is safe by construction. LobSource wrappers are moved into the frame
/// (passing `halcyon::lobFile(path)` inline is the expected idiom); their
/// REFERENTS must stay alive until the returned Task completes. Inherits
/// auto-retry, reconnect, metrics, tracing, and logging unchanged.
template <class... Args>
Task<Result<std::int64_t>> execute(const Database& db, std::string sql,
                                   Args&&... args) {
    auto backing = db.asyncBacking();
    auto owned = std::make_tuple(detail::own_arg(std::forward<Args>(args))...);
    return detail::execute_task(std::move(backing), std::move(sql),
                                std::move(owned));
}

/// \brief Awaitable Database::execute with named parameters (`params` owns
/// its values already).
inline Task<Result<std::int64_t>> execute(const Database& db, std::string sql,
                                          params named) {
    return detail::execute_named_task(db.asyncBacking(), std::move(sql),
                                      std::move(named));
}

/// \brief Awaitable typed query. Materializes rows into std::vector<T>
/// exactly like queryAsync — the task carries no pool lease.
template <class T, class... Args>
Task<Result<std::vector<T>>> query(const Database& db, std::string sql,
                                   Args&&... args) {
    auto backing = db.asyncBacking();
    auto owned = std::make_tuple(detail::own_arg(std::forward<Args>(args))...);
    return detail::query_task<T>(std::move(backing), std::move(sql),
                                 std::move(owned));
}

template <class T>
Task<Result<std::vector<T>>> query(const Database& db, std::string sql,
                                   params named) {
    return detail::query_named_task<T>(db.asyncBacking(), std::move(sql),
                                       std::move(named));
}

/// \brief Awaitable batch execute; build the Batch with the batchOf helpers.
/// No auto-retry (matches the sync executeBatch), no LOB sources (recorded
/// backlog item).
inline Task<Result<std::int64_t>> executeBatch(const Database& db,
                                               std::string sql, Batch batch) {
    return detail::execute_batch_task(db.asyncBacking(), std::move(sql),
                                      std::move(batch));
}

/// \brief Awaitable functional transaction (optionally at an isolation
/// level). `fn` is either a plain callable returning Result<T> — it runs
/// entirely on one worker; use the normal blocking tx.execute/tx.query inside
/// — or a Task<Result<T>>-returning coroutine, which may await non-Halcyon
/// awaitables mid-transaction (commit/rollback are hopped back onto a Halcyon
/// worker; keep transactional scopes short — the pooled connection is held
/// across every await).
///
/// Tracing: the plain-callable path emits the usual "halcyon.transaction"
/// span. The coroutine-fn path does NOT (documented limitation — it cannot
/// use the instrumented sync scope across foreign-thread hops); context
/// propagation and per-operation instrumentation are unaffected.
template <class Fn>
auto transaction(const Database& db, Fn fn) {
    using R0 = std::invoke_result_t<Fn&, Transaction&>;
    if constexpr (detail::is_task<R0>::value) {
        using R = typename detail::task_payload<R0>::type;
        return detail::transaction_coro_task<Fn, R>(db.asyncBacking(),
                                                    std::nullopt, std::move(fn));
    } else {
        return detail::transaction_plain_task<Fn, R0>(
            db.asyncBacking(), std::nullopt, std::move(fn));
    }
}

template <class Fn>
auto transaction(const Database& db, Isolation iso, Fn fn) {
    using R0 = std::invoke_result_t<Fn&, Transaction&>;
    if constexpr (detail::is_task<R0>::value) {
        using R = typename detail::task_payload<R0>::type;
        return detail::transaction_coro_task<Fn, R>(
            db.asyncBacking(), std::optional<Isolation>(iso), std::move(fn));
    } else {
        return detail::transaction_plain_task<Fn, R0>(
            db.asyncBacking(), std::optional<Isolation>(iso), std::move(fn));
    }
}

}  // namespace halcyon::coro

#endif  // HALCYON_HAS_CORO_SUPPORT
