#pragma once

#include "halcyon/coro/detail/require_coroutines.hpp"

#if HALCYON_HAS_CORO_SUPPORT  // swallow everything after the guard's #error

#include <cassert>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "halcyon/async.hpp"

namespace halcyon::coro {

template <class T>
class Task;

namespace detail {

// Continuation slot + symmetric-transfer completion shared by both promises.
struct PromiseBase {
    std::coroutine_handle<> continuation;

    struct FinalAwaiter {
        bool await_ready() const noexcept { return false; }
        template <class P>
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<P> h) const noexcept {
            // Symmetric transfer into the awaiter: deep task chains complete
            // without growing the native stack.
            auto c = h.promise().continuation;
            return c ? c : std::noop_coroutine();
        }
        void await_resume() const noexcept {}
    };

    std::suspend_always initial_suspend() const noexcept { return {}; }  // lazy
    FinalAwaiter final_suspend() const noexcept { return {}; }
};

template <class T>
struct TaskPromise final : PromiseBase {
    // monostate = still running; index 1 = value; index 2 = thrown.
    std::variant<std::monostate, T, std::exception_ptr> outcome;

    Task<T> get_return_object() noexcept;
    void return_value(T v) { outcome.template emplace<1>(std::move(v)); }
    void unhandled_exception() noexcept {
        outcome.template emplace<2>(std::current_exception());
    }
    T take() {
        if (outcome.index() == 2)
            std::rethrow_exception(std::get<2>(std::move(outcome)));
        return std::get<1>(std::move(outcome));
    }
};

template <>
struct TaskPromise<void> final : PromiseBase {
    std::exception_ptr error;

    Task<void> get_return_object() noexcept;
    void return_void() const noexcept {}
    void unhandled_exception() noexcept { error = std::current_exception(); }
    void take() {
        if (error) std::rethrow_exception(error);
    }
};

}  // namespace detail

/// \brief Lazy, move-only, single-consumer coroutine task (spec v1.2 §A.2).
///
/// Nothing runs until the task is co_awaited (or handed to syncWait); a task
/// that is never awaited never runs and destroys cleanly. Await a given task
/// at most once. Exceptions thrown inside the task propagate out of co_await —
/// the throwing style is the idiom `(co_await op).value()`.
template <class T = void>
class [[nodiscard]] Task {
public:
    using promise_type = detail::TaskPromise<T>;

    Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            destroy();
            h_ = std::exchange(o.h_, {});
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { destroy(); }

    auto operator co_await() noexcept {
        struct Awaiter {
            std::coroutine_handle<promise_type> h;
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<> awaiting) const noexcept {
                h.promise().continuation = awaiting;
                return h;  // symmetric transfer: start the task now
            }
            T await_resume() const { return h.promise().take(); }
        };
        assert(h_ && "co_await on an empty (moved-from) Task");
        return Awaiter{h_};
    }

private:
    template <class U>
    friend struct detail::TaskPromise;

    explicit Task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}
    void destroy() noexcept {
        if (h_) {
            h_.destroy();
            h_ = {};
        }
    }

    std::coroutine_handle<promise_type> h_;
};

namespace detail {

template <class T>
Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>(std::coroutine_handle<TaskPromise<T>>::from_promise(*this));
}
inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>(
        std::coroutine_handle<TaskPromise<void>>::from_promise(*this));
}

// --- syncWait plumbing ---

struct SyncWaitSignal {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;

    void set() {
        // Notify UNDER the lock: the waiter may destroy this object the moment
        // it observes done, so notify_one must complete before it can wake.
        std::lock_guard<std::mutex> lk(m);
        done = true;
        cv.notify_one();
    }
    void wait() {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return done; });
    }
};

template <class T>
struct SyncOutcome {
    std::optional<T> value;
    std::exception_ptr error;
    T take() {
        if (error) std::rethrow_exception(error);
        return std::move(*value);
    }
};
template <>
struct SyncOutcome<void> {
    std::exception_ptr error;
    void take() {
        if (error) std::rethrow_exception(error);
    }
};

// Eager, self-destroying driver coroutine used only by syncWait. Its body
// catches everything, so unhandled_exception is unreachable.
struct Detached {
    struct promise_type {
        Detached get_return_object() const noexcept { return {}; }
        std::suspend_never initial_suspend() const noexcept { return {}; }
        std::suspend_never final_suspend() const noexcept { return {}; }
        void return_void() const noexcept {}
        void unhandled_exception() const noexcept { std::terminate(); }
    };
};

template <class T>
Detached sync_wait_driver(Task<T> t, SyncOutcome<T>& out, SyncWaitSignal& sig) {
    {
        Task<T> task = std::move(t);
        try {
            if constexpr (std::is_void_v<T>) {
                co_await std::move(task);
            } else {
                out.value.emplace(co_await std::move(task));
            }
        } catch (...) {
            out.error = std::current_exception();
        }
    }  // the awaited task — and everything its frame owns (leases, cursors,
       // transactions) — is destroyed HERE, before the waiter can observe
       // completion; syncWait returning therefore means full teardown ran
    sig.set();  // last touch of out/sig; the driver frame self-destroys after
}

}  // namespace detail

/// \brief Runs a task to completion from synchronous (non-coroutine) code and
/// returns its value (or rethrows its exception).
///
/// NEVER call this from a Halcyon executor worker thread (i.e. from inside a
/// continuation after any co_await): the awaited task needs a worker to
/// complete, so blocking one can self-deadlock when the pool is saturated.
/// Debug builds assert.
template <class T>
T syncWait(Task<T> task) {
    assert(!Executor::onWorkerThread() &&
           "coro::syncWait called on a Halcyon executor worker thread "
           "(self-deadlock risk)");
    detail::SyncOutcome<T> out;
    detail::SyncWaitSignal sig;
    detail::sync_wait_driver(std::move(task), out, sig);
    sig.wait();
    return out.take();
}

}  // namespace halcyon::coro

#endif  // HALCYON_HAS_CORO_SUPPORT
