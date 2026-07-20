#pragma once

#include "halcyon/coro/detail/require_coroutines.hpp"

#if HALCYON_HAS_CORO_SUPPORT  // swallow everything after the guard's #error

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "halcyon/coro/detail/offload.hpp"
#include "halcyon/coro/task.hpp"
#include "halcyon/database.hpp"
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

}  // namespace halcyon::coro

#endif  // HALCYON_HAS_CORO_SUPPORT
