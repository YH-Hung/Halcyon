#include "workload_runner.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace halcyon::stress {

namespace {

// A simple reusable start gate: workers wait until release() is called once.
class StartGate {
public:
    void wait() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return open_; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            open_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool open_ = false;
};

// A single-use N-thread barrier. The last thread to arrive runs on_last() (under
// the lock, before any waiter wakes) so it can publish state that every released
// thread is then guaranteed to observe. Used to separate the warmup phase from the
// timed phase: warmup measurements are discarded and the timed window starts only
// once every worker has finished warming up.
class Barrier {
public:
    explicit Barrier(std::size_t n) : count_(n), total_(n) {}

    template <class F>
    void arrive_and_wait(F&& on_last) {
        std::unique_lock<std::mutex> lk(mu_);
        const std::size_t gen = gen_;
        if (--count_ == 0) {
            on_last();
            count_ = total_;
            ++gen_;
            cv_.notify_all();
        } else {
            cv_.wait(lk, [&] { return gen_ != gen; });
        }
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::size_t count_;
    std::size_t total_;
    std::size_t gen_ = 0;
};

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

}  // namespace

RunReport run_workload(const Workload& w, const RunConfig& cfg) {
    const std::size_t nthreads = cfg.threads == 0 ? 1 : cfg.threads;
    const bool duration_mode = cfg.stop.duration.has_value();

    StartGate gate;
    Barrier warmup_done(nthreads);
    std::vector<WorkerCtx> ctxs(nthreads);
    std::vector<std::thread> workers;
    workers.reserve(nthreads);

    // Iteration budgets (shared countdowns so totals are exact regardless of
    // scheduling): one for the discarded warmup phase, one for the timed phase.
    std::atomic<long long> warmup_remaining{
        static_cast<long long>(cfg.warmup_iters)};
    std::atomic<long long> timed_remaining{
        cfg.stop.total_iters ? static_cast<long long>(*cfg.stop.total_iters) : -1};

    // Warmup deadline (duration mode) is relative to gate release; the timed
    // deadline/start are published by the warmup barrier so the timed window
    // excludes warmup and starts in lockstep across threads.
    std::chrono::steady_clock::time_point warmup_deadline;
    std::chrono::steady_clock::time_point timed_deadline;
    std::atomic<std::uint64_t> timed_start_ns{0};

    auto worker = [&](std::size_t idx) {
        WorkerCtx& ctx = ctxs[idx];
        ctx.thread_index = idx;
        ctx.rng.seed(cfg.seed ^ (0x9e3779b97f4a7c15ull * (idx + 1)));
        gate.wait();

        // --- warmup phase (untimed, discarded) ---
        if (duration_mode) {
            while (cfg.warmup_duration.count() > 0 && !ctx.failed &&
                   std::chrono::steady_clock::now() < warmup_deadline)
                w.body(ctx);
        } else {
            while (!ctx.failed && warmup_remaining.fetch_sub(1) > 0)
                w.body(ctx);
        }

        // Discard everything the warmup produced; only the timed window counts.
        ctx.hist = LatencyHistogram{};
        ctx.errors = 0;

        // All workers meet here; the last one starts the timed window for all.
        warmup_done.arrive_and_wait([&] {
            timed_start_ns.store(now_ns(), std::memory_order_relaxed);
            if (duration_mode)
                timed_deadline =
                    std::chrono::steady_clock::now() + *cfg.stop.duration;
        });

        // --- timed phase ---
        for (;;) {
            if (ctx.failed) return;
            if (duration_mode) {
                if (std::chrono::steady_clock::now() >= timed_deadline) return;
            } else {
                if (timed_remaining.fetch_sub(1) <= 0) return;
            }
            const auto t0 = std::chrono::steady_clock::now();
            w.body(ctx);
            const auto t1 = std::chrono::steady_clock::now();
            ctx.hist.record(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count()));
        }
    };

    for (std::size_t i = 0; i < nthreads; ++i)
        workers.emplace_back(worker, i);

    const auto release = std::chrono::steady_clock::now();
    if (duration_mode) warmup_deadline = release + cfg.warmup_duration;
    gate.release();
    for (auto& t : workers) t.join();
    const std::uint64_t end_ns = now_ns();

    RunReport r;
    r.name = w.name;
    r.threads = nthreads;
    const std::uint64_t started = timed_start_ns.load(std::memory_order_relaxed);
    r.wall = std::chrono::nanoseconds(end_ns > started ? end_ns - started : 0);
    for (const auto& ctx : ctxs) {
        r.hist.merge(ctx.hist);
        r.errors += ctx.errors;
        if (ctx.failed && !r.failed) {
            r.failed = true;
            r.first_error = ctx.error;
        }
    }
    r.ops = r.hist.count();
    return r;
}

}  // namespace halcyon::stress
