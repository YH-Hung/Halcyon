#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "halcyon/connection.hpp"
#include "halcyon/pool.hpp"

namespace halcyon {

namespace detail {
// Per-thread flag: true while the current thread is a Halcyon executor worker.
// Function-local so the inline function is one entity across all TUs.
inline bool& executor_worker_flag() {
    thread_local bool is_worker = false;
    return is_worker;
}
}  // namespace detail

/// \brief Fixed-size thread pool backing `Database::executeAsync` / `queryAsync`
/// and the C++20 coroutine layer (halcyon/coro.hpp).
///
/// Submit any callable via `submit(fn)` — returns a `std::future<ReturnType>`.
/// Drains in-flight tasks on destruction. Teardown is safe on ANY thread,
/// including this executor's own workers: the destructor joins every worker
/// except the current thread and detaches itself when it *is* a worker.
class Executor {
public:
    // Internal shared block co-owned by this handle and by every worker thread
    // (each worker holds a shared_ptr for its whole lifetime, so the loop's
    // post-destructor member access is always backed memory). The coroutine
    // layer references the executor ONLY through a weak_ptr to this block:
    // releasing a strong State reference controls no thread lifetimes, so it
    // is safe on any thread at any time. Public for the coro layer; not part
    // of the supported user API.
    struct State {
        std::mutex mu;
        std::condition_variable cv;
        std::queue<std::function<void()>> tasks;
        bool stop = false;

        // Fire-and-forget enqueue, stop-aware and atomic: enqueued-or-rejected,
        // never stranded. `stop` is only ever set under `mu`, and workers exit
        // only after observing stop-and-empty-queue under `mu`, so a job
        // enqueued before stop is guaranteed drained, and a post after stop is
        // rejected (returns false; the job is NOT enqueued).
        //
        // NOEXCEPT-CALLABLE CONTRACT: the worker loop invokes jobs bare, so a
        // throwing job would escape the thread function and terminate the
        // process. Mark the lambda noexcept and contain exceptions inside it.
        template <class Fn>
        bool post(Fn&& fn) {
            static_assert(std::is_nothrow_invocable_v<std::decay_t<Fn>&>,
                          "Executor::post requires a noexcept-invocable job: "
                          "mark the lambda noexcept and catch everything inside");
            {
                std::lock_guard<std::mutex> lk(mu);
                if (stop) return false;
                tasks.emplace(std::forward<Fn>(fn));
            }
            cv.notify_one();
            return true;
        }
    };

    explicit Executor(std::size_t threads) : state_(std::make_shared<State>()) {
        if (threads == 0) threads = 1;
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i)
            workers_.emplace_back([s = state_]() mutable {
                worker_loop(std::move(s));
            });
    }

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    // Safe on any thread, including this executor's own workers (a user
    // continuation resumed on a worker can destroy the last Database copy and,
    // with it, this handle). A self-join would deadlock; instead the current
    // thread is detached and keeps draining over the co-owned State after this
    // destructor returns. Joined workers exit only at stop-and-empty-queue, so
    // every job enqueued before stop is drained either way.
    ~Executor() {
        {
            std::lock_guard<std::mutex> lk(state_->mu);
            state_->stop = true;
        }
        state_->cv.notify_all();
        const auto self = std::this_thread::get_id();
        for (auto& t : workers_) {
            if (!t.joinable()) continue;
            if (t.get_id() == self)
                t.detach();  // teardown on a worker: never self-join
            else
                t.join();
        }
    }

    template <class Fn>
    auto submit(Fn&& fn) -> std::future<std::invoke_result_t<std::decay_t<Fn>>> {
        using R = std::invoke_result_t<std::decay_t<Fn>>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<Fn>(fn));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(state_->mu);
            state_->tasks.emplace([task] { (*task)(); });
        }
        state_->cv.notify_one();
        return fut;
    }

    // Fire-and-forget submission (no future/packaged_task); see State::post
    // for the stop/reject semantics and the noexcept-callable contract.
    template <class Fn>
    bool post(Fn&& fn) {
        return state_->post(std::forward<Fn>(fn));
    }

    // Weak reference to the shared State for the coroutine layer: awaitables
    // lock it at await time and post through it, never holding (or even
    // briefly taking) a strong reference to this handle.
    std::weak_ptr<State> weakState() const noexcept { return state_; }

    // True when the calling thread is a Halcyon executor worker (any
    // executor). Used by coro::syncWait's self-deadlock debug assert and
    // available to users writing their own guards.
    static bool onWorkerThread() noexcept {
        return detail::executor_worker_flag();
    }

private:
    // Static and shared_ptr-parameterized: after ~Executor detaches this
    // thread, the loop touches only the co-owned State, never the freed handle.
    static void worker_loop(std::shared_ptr<State> s) {
        detail::executor_worker_flag() = true;
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(s->mu);
                s->cv.wait(lk, [&] { return s->stop || !s->tasks.empty(); });
                if (s->tasks.empty()) {
                    if (s->stop) break;  // stop observed with an empty queue
                    continue;
                }
                job = std::move(s->tasks.front());
                s->tasks.pop();
            }
            job();
        }
        detail::executor_worker_flag() = false;
    }

    std::shared_ptr<State> state_;
    std::vector<std::thread> workers_;
};

/// \brief Async helper: acquires a pooled connection on a worker thread and runs `fn(Connection&)`.
///
/// `fn` must return `Result<T>`; an acquire failure short-circuits to that error.
/// Returns `std::future<Result<T>>`.
template <class Fn>
auto async_with_connection(Executor& ex, ConnectionPool& pool, Fn fn)
    -> std::future<std::decay_t<decltype(fn(std::declval<Connection&>()))>> {
    using R = std::decay_t<decltype(fn(std::declval<Connection&>()))>;
    return ex.submit([&pool, fn = std::move(fn)]() mutable -> R {
        auto pc = pool.acquire();
        if (!pc.ok()) return R(pc.error());
        return fn(*pc.value());
    });
}

}  // namespace halcyon
