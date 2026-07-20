#pragma once

#include "halcyon/coro/detail/require_coroutines.hpp"

#if HALCYON_HAS_CORO_SUPPORT  // swallow everything after the guard's #error

#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "halcyon/database.hpp"
#include "halcyon/error.hpp"
#include "halcyon/lob.hpp"

namespace halcyon::coro::detail {

inline Error executor_gone_error() {
    Error e;
    e.code = ErrorCode::InvalidState;
    e.message =
        "halcyon executor unavailable: every Database copy was destroyed (or "
        "is stopping) before this task was awaited";
    return e;
}

// One offloaded blocking call (spec §A.2), entirely inside await_suspend on
// the awaiting thread:
//   1. lock the bundle's weak_ptr<Executor::State>; on failure complete the
//      await immediately (no suspension) with InvalidState;
//   2. post a noexcept closure that — on the worker — attaches the bundle's
//      trace context, runs `op` against the sync view inside a catch-all
//      (result or std::exception_ptr stored into this awaitable, which lives
//      in the suspended frame), and resumes the coroutine handle. A rejected
//      post (stop already set) completes the await with InvalidState before
//      suspension commits — the coroutine is never left suspended;
//   3. drop the State reference (a strong State ref controls no thread
//      lifetimes, so releasing it anywhere is safe).
// The catch-all is what satisfies post's noexcept-callable contract while
// preserving "exceptions propagate out of co_await".
//
// R must be a Result<...> (constructible from Error). No strong reference to
// the Executor HANDLE is ever taken — not even briefly.
template <class R, class Op>
class OffloadAwaitable {
    static_assert(std::is_constructible_v<R, Error>,
                  "offload ops must return a Result<...>");

public:
    OffloadAwaitable(Database::AsyncBacking& backing, Op op)
        : backing_(&backing), op_(std::move(op)) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
        auto state = backing_->exec.lock();
        if (!state) {
            rejected_ = true;
            return false;  // resume immediately; await_resume reports the error
        }
        const bool posted = state->post([this, h]() noexcept {
            run_and_resume(h);
        });
        if (!posted) {
            rejected_ = true;  // stop already set: never stranded
            return false;
        }
        return true;  // suspended; the frame must not be touched from here
    }

    R await_resume() {
        if (rejected_) return R(executor_gone_error());
        if (error_) std::rethrow_exception(error_);
        return std::move(*outcome_);
    }

private:
    // Everything the worker does, under post's noexcept contract. The context
    // guard is attached for the WHOLE body: the op's spans parent under it,
    // AND it must still be active while h.resume() runs the continuation —
    // user code after co_await (including the next factory's captureContext)
    // executes on this worker and must observe the original context, or a
    // chained op would capture an orphan. The guard is a LOCAL (worker
    // stack), never frame state, so destroying it after resume() is safe
    // even if the frame is gone by then. attachContext is user adapter code
    // and may throw: a failure is caught into the frame's exception_ptr and
    // rethrown at co_await (the op is skipped) — never std::terminate. The
    // catch wraps ONLY the attachment so h.resume() runs exactly once, after
    // it, with the guard (attached or empty) still in scope.
    void run_and_resume(std::coroutine_handle<> h) noexcept {
        obs::ScopedContext guard;
        try {
            guard = backing_->syncView.useParentContext(backing_->traceContext);
        } catch (...) {
            error_ = std::current_exception();
        }
        if (!error_) run_and_store();
        h.resume();  // exactly once; context (if attached) active throughout
    }

    void run_and_store() noexcept {
        try {
            outcome_.emplace(op_());
        } catch (...) {
            error_ = std::current_exception();
        }
    }

    Database::AsyncBacking* backing_;
    Op op_;
    std::optional<R> outcome_;
    std::exception_ptr error_;
    bool rejected_ = false;
};

template <class Op>
auto offload(Database::AsyncBacking& backing, Op op) {
    using R = std::invoke_result_t<Op&>;
    return OffloadAwaitable<R, Op>(backing, std::move(op));
}

// Factory-time materialization (spec §A.3): ordinary parameters become OWNED
// seam Values now (string_view / const char* are copied into owning storage —
// a decay-copy alone would still be a view); a LobSource WRAPPER is moved into
// the frame as-is (only its referents — lobStream's stream, lobCallback
// reference captures — are borrowed and must outlive the task).
template <class T>
auto own_arg(T&& v) {
    using D = std::decay_t<T>;
    if constexpr (is_lob_source<D>::value) {
        return D(std::forward<T>(v));
    } else {
        return halcyon::detail::to_value(v);
    }
}

}  // namespace halcyon::coro::detail

#endif  // HALCYON_HAS_CORO_SUPPORT
