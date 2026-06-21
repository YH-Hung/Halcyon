#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>

#include "latency_histogram.hpp"

namespace halcyon::stress {

// Stop after a fixed number of iterations (deterministic; correctness) OR after a
// wall-clock duration (steady-state; perf). Exactly one should be set.
struct Stop {
    std::optional<std::size_t> total_iters;
    std::optional<std::chrono::milliseconds> duration;
};

struct RunConfig {
    std::size_t threads = 1;
    Stop stop;
    std::size_t warmup_iters = 0;  // perf: discarded before timing starts
    std::uint64_t seed = 0;
};

// Per-worker state passed to the workload body. Each worker owns its RNG and
// histogram (no shared per-op state -> no false contention, no measurement skew).
struct WorkerCtx {
    std::size_t thread_index = 0;
    std::mt19937_64 rng;
    LatencyHistogram hist;

    // Record the first failure for this worker and stop its loop.
    void fail(std::string msg) {
        if (!failed) {
            failed = true;
            error = std::move(msg);
        }
    }
    bool failed = false;
    std::string error;
};

struct RunReport {
    std::string name;
    std::size_t threads = 0;
    std::uint64_t ops = 0;
    std::chrono::nanoseconds wall{0};
    LatencyHistogram hist;
    bool failed = false;
    std::string first_error;

    double throughput_per_sec() const {
        const double secs =
            std::chrono::duration<double>(wall).count();
        return secs > 0.0 ? static_cast<double>(ops) / secs : 0.0;
    }
    double p50_us() const { return hist.percentile_us(0.50); }
    double p95_us() const { return hist.percentile_us(0.95); }
    double p99_us() const { return hist.percentile_us(0.99); }
    double max_us() const { return hist.max_us(); }
};

struct Workload {
    std::string name;
    std::function<void(WorkerCtx&)> body;
};

// Spawns cfg.threads workers, releases them from a barrier together, loops the
// body until the stop condition, times each call into the worker histogram, joins
// and merges. The first worker failure is surfaced in the report.
RunReport run_workload(const Workload& w, const RunConfig& cfg);

}  // namespace halcyon::stress
