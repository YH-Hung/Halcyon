#pragma once

#include "halcyon/coro/detail/require_coroutines.hpp"

#if HALCYON_HAS_CORO_SUPPORT  // swallow everything after the guard's #error

#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "halcyon/coro/detail/offload.hpp"
#include "halcyon/coro/task.hpp"
#include "halcyon/database.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/streaming.hpp"

namespace halcyon::coro {

class StreamingRow;
class LobReader;

namespace detail {
struct StreamingQueryAccess;
}  // namespace detail

/// \brief Awaitable streaming cursor: wraps and owns the v1.1
/// StreamingQueryResult (and thereby the connection lease). Move-only,
/// single-owner.
///
/// BORROW RULES (spec §A.5 — explicit because lazy awaitables can violate
/// them in ways the sync API cannot): next(), LobReader::read(), and the
/// drain helpers return single-use awaitables/tasks that borrow this object —
/// it must stay alive, unmoved, from the start of such an await until it
/// completes. Await immediately (`co_await sq.next()`); storing a member
/// awaitable for later is unsupported. A StreamingRow borrows its
/// StreamingQuery and is invalidated by the next next() — the same rule as
/// the v1.1 sync row. Ascending-column-order and NULL-LOB semantics are
/// inherited from v1.1 unchanged.
class StreamingQuery {
public:
    StreamingQuery(StreamingQuery&&) noexcept = default;
    StreamingQuery& operator=(StreamingQuery&&) noexcept = default;
    StreamingQuery(const StreamingQuery&) = delete;
    StreamingQuery& operator=(const StreamingQuery&) = delete;

    class NextAwaitable;

    /// co_await -> std::optional<StreamingRow>. nullopt means end OR error —
    /// disambiguate with ok()/error() after the loop, exactly like the sync
    /// streaming API. The row fetch offloads to the executor (it hits the
    /// server); scalar cell reads on the returned row stay synchronous.
    NextAwaitable next() noexcept;

    std::size_t column_count() const noexcept { return rs_.column_count(); }
    bool ok() const noexcept { return !localError_.has_value() && rs_.ok(); }
    const std::optional<Error>& error() const noexcept {
        return localError_.has_value() ? localError_ : rs_.error();
    }

private:
    friend class StreamingRow;
    friend class LobReader;
    friend struct detail::StreamingQueryAccess;

    StreamingQuery(Database::AsyncBacking backing, StreamingQueryResult rs)
        : backing_(std::move(backing)), rs_(std::move(rs)) {}

    Database::AsyncBacking backing_;
    StreamingQueryResult rs_;
    std::optional<Error> localError_;  // coro-layer failures (executor gone)
};

/// \brief Single-use awaitable returned by StreamingQuery::next(). Borrows
/// the StreamingQuery; the worker stores the fetched row (or exception) here
/// in the suspended frame and resumes the coroutine on itself.
class StreamingQuery::NextAwaitable {
public:
    explicit NextAwaitable(StreamingQuery* sq) noexcept : sq_(sq) {}
    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
        auto state = sq_->backing_.exec.lock();
        if (!state) {
            rejected_ = true;
            return false;
        }
        const bool posted = state->post([this, h]() noexcept {
            // Context attached across the resume (see OffloadAwaitable's
            // run_and_resume): the continuation after co_await sq.next() runs
            // on this worker and may call the next factory. The guard is
            // closure-local — safe to destroy after resume(). attachContext
            // is user adapter code and may throw: caught into error_ and
            // rethrown at co_await (the fetch is skipped), never terminate;
            // h.resume() runs exactly once, outside every try.
            obs::ScopedContext guard;
            try {
                guard = sq_->backing_.syncView.useParentContext(
                    sq_->backing_.traceContext);
            } catch (...) {
                error_ = std::current_exception();
            }
            if (!error_) {
                try {
                    row_ = sq_->rs_.next();
                } catch (...) {
                    error_ = std::current_exception();
                }
            }
            h.resume();
        });
        if (!posted) {
            rejected_ = true;
            return false;
        }
        return true;
    }
    std::optional<StreamingRow> await_resume();

private:
    StreamingQuery* sq_;
    std::optional<halcyon::StreamingRow> row_;
    std::exception_ptr error_;
    bool rejected_ = false;
};

/// \brief Borrowed view of the current row; invalidated by the next next().
class StreamingRow {
public:
    std::size_t column_count() const noexcept { return row_.column_count(); }

    /// Scalar cells stay synchronous: the data was fetched with the row —
    /// no executor hop.
    template <class T>
    Result<T> get(std::size_t col) {
        return row_.template get<T>(col);
    }

    /// Opens chunked awaitable access to a LOB cell. Returns Result because
    /// invalid or out-of-order columns fail recoverably (mirrors v1.1).
    Result<LobReader> lob(std::size_t col);

private:
    friend class StreamingQuery::NextAwaitable;
    StreamingRow(StreamingQuery* sq, halcyon::StreamingRow row)
        : sq_(sq), row_(row) {}
    StreamingQuery* sq_;
    halcyon::StreamingRow row_;
};

/// \brief Awaitable chunked reader over one LOB cell (0 ⇒ EOF; isNull() is
/// meaningful after the first read — a SQL NULL reads as immediate EOF with
/// isNull() true). read() offloads per chunk; each drain helper loops ONCE on
/// a worker (a single offload for the whole drain). Borrows its
/// StreamingQuery — and read() borrows the caller's buffer — until each
/// returned Task completes.
class LobReader {
public:
    Task<Result<std::size_t>> read(std::byte* buf, std::size_t len) {
        auto* self = this;
        co_return co_await detail::offload(
            self->sq_->backing_,
            [self, buf, len] { return self->inner_.read(buf, len); });
    }
    bool isNull() const noexcept { return inner_.isNull(); }

    Task<Result<std::vector<std::byte>>> toVector() {
        auto* self = this;
        co_return co_await detail::offload(
            self->sq_->backing_, [self] { return self->inner_.toVector(); });
    }
    Task<Result<std::string>> toString() {
        auto* self = this;
        co_return co_await detail::offload(
            self->sq_->backing_, [self] { return self->inner_.toString(); });
    }
    Task<Result<void>> toFile(std::string path) {
        auto* self = this;
        co_return co_await detail::offload(
            self->sq_->backing_,
            [self, path] { return self->inner_.toFile(path); });
    }

private:
    friend class StreamingRow;
    LobReader(StreamingQuery* sq, halcyon::LobReader inner)
        : sq_(sq), inner_(inner) {}
    StreamingQuery* sq_;
    halcyon::LobReader inner_;
};

inline Result<LobReader> StreamingRow::lob(std::size_t col) {
    auto r = row_.lob(col);
    if (!r.ok()) return r.error();
    return LobReader(sq_, r.value());
}

inline std::optional<StreamingRow>
StreamingQuery::NextAwaitable::await_resume() {
    if (rejected_) {
        sq_->localError_ = detail::executor_gone_error();
        return std::nullopt;
    }
    if (error_) std::rethrow_exception(error_);
    if (!row_.has_value()) return std::nullopt;
    return StreamingRow(sq_, *row_);
}

inline StreamingQuery::NextAwaitable StreamingQuery::next() noexcept {
    return NextAwaitable(this);
}

namespace detail {

struct StreamingQueryAccess {
    static StreamingQuery make(Database::AsyncBacking backing,
                               StreamingQueryResult rs) {
        return StreamingQuery(std::move(backing), std::move(rs));
    }
};

template <class Owned>
Task<Result<StreamingQuery>> query_streaming_task(
    Database::AsyncBacking backing, std::string sql, Owned owned) {
    auto rs = co_await offload(backing, [&] {
        return std::apply(
            [&](const auto&... vs) {
                return backing.syncView.queryStreaming(sql, vs...);
            },
            owned);
    });
    if (!rs.ok()) co_return Result<StreamingQuery>(rs.error());
    co_return Result<StreamingQuery>(
        StreamingQueryAccess::make(std::move(backing), std::move(rs.value())));
}

inline Task<Result<StreamingQuery>> query_streaming_named_task(
    Database::AsyncBacking backing, std::string sql, params named) {
    auto rs = co_await offload(
        backing, [&] { return backing.syncView.queryStreaming(sql, named); });
    if (!rs.ok()) co_return Result<StreamingQuery>(rs.error());
    co_return Result<StreamingQuery>(
        StreamingQueryAccess::make(std::move(backing), std::move(rs.value())));
}

}  // namespace detail

/// \brief Awaitable streaming query (row-at-a-time cursor, chunked LOB
/// reads). Single attempt — streaming queries are never auto-retried,
/// matching the sync API. The returned StreamingQuery owns the lease; keep it
/// alive while iterating.
template <class... Args>
Task<Result<StreamingQuery>> queryStreaming(const Database& db,
                                            std::string sql, Args&&... args) {
    auto backing = db.asyncBacking();
    auto owned = std::make_tuple(detail::own_arg(std::forward<Args>(args))...);
    return detail::query_streaming_task(std::move(backing), std::move(sql),
                                        std::move(owned));
}

inline Task<Result<StreamingQuery>> queryStreaming(const Database& db,
                                                   std::string sql,
                                                   params named) {
    return detail::query_streaming_named_task(db.asyncBacking(),
                                              std::move(sql), std::move(named));
}

}  // namespace halcyon::coro

#endif  // HALCYON_HAS_CORO_SUPPORT
