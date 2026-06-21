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

}  // namespace

RunReport run_workload(const Workload& w, const RunConfig& cfg) {
    const std::size_t nthreads = cfg.threads == 0 ? 1 : cfg.threads;

    StartGate gate;
    std::vector<WorkerCtx> ctxs(nthreads);
    std::vector<std::thread> workers;
    workers.reserve(nthreads);

    // Iteration budget (total_iters mode): a shared countdown shared by all
    // workers so the total op count is exact regardless of thread scheduling.
    std::atomic<long long> remaining{
        cfg.stop.total_iters ? static_cast<long long>(*cfg.stop.total_iters) : -1};

    const bool duration_mode = cfg.stop.duration.has_value();
    std::chrono::steady_clock::time_point deadline;

    auto worker = [&](std::size_t idx) {
        WorkerCtx& ctx = ctxs[idx];
        ctx.thread_index = idx;
        ctx.rng.seed(cfg.seed ^ (0x9e3779b97f4a7c15ull * (idx + 1)));
        gate.wait();
        for (;;) {
            if (ctx.failed) return;
            if (duration_mode) {
                if (std::chrono::steady_clock::now() >= deadline) return;
            } else {
                if (remaining.fetch_sub(1) <= 0) return;
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

    const auto start = std::chrono::steady_clock::now();
    if (duration_mode) deadline = start + *cfg.stop.duration;
    gate.release();
    for (auto& t : workers) t.join();
    const auto end = std::chrono::steady_clock::now();

    RunReport r;
    r.name = w.name;
    r.threads = nthreads;
    r.wall = end - start;
    for (const auto& ctx : ctxs) {
        r.hist.merge(ctx.hist);
        if (ctx.failed && !r.failed) {
            r.failed = true;
            r.first_error = ctx.error;
        }
    }
    r.ops = r.hist.count();
    return r;
}

}  // namespace halcyon::stress
