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

/// \brief Fixed-size thread pool backing `Database::executeAsync` / `queryAsync`.
///
/// Submit any callable via `submit(fn)` — returns a `std::future<ReturnType>`.
/// A future coroutine `task<T>`/awaitable layer can wrap `submit()` without
/// changing this API. Drains in-flight tasks on destruction.
class Executor {
public:
    explicit Executor(std::size_t threads) {
        if (threads == 0) threads = 1;
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i)
            workers_.emplace_back([this] { worker_loop(); });
    }

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    ~Executor() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
    }

    template <class Fn>
    auto submit(Fn&& fn) -> std::future<std::invoke_result_t<std::decay_t<Fn>>> {
        using R = std::invoke_result_t<std::decay_t<Fn>>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<Fn>(fn));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(mu_);
            tasks_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                if (tasks_.empty()) {
                    if (stop_) return;
                    continue;
                }
                job = std::move(tasks_.front());
                tasks_.pop();
            }
            job();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
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
