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

private:
    // Static and shared_ptr-parameterized: after ~Executor detaches this
    // thread, the loop touches only the co-owned State, never the freed handle.
    static void worker_loop(std::shared_ptr<State> s) {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(s->mu);
                s->cv.wait(lk, [&] { return s->stop || !s->tasks.empty(); });
                if (s->tasks.empty()) {
                    if (s->stop) return;  // stop observed with an empty queue
                    continue;
                }
                job = std::move(s->tasks.front());
                s->tasks.pop();
            }
            job();
        }
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
