# Halcyon Concurrency Stress & Performance Suite — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Build a concurrency stress & performance suite that proves Halcyon is race/deadlock-free under high concurrency (assertion + sanitizer driven) and reports its throughput/latency/scaling.

**Architecture:** One shared `tests/stress/support/` core — a thread-safe `ConcurrentFakeDriver`, a `LatencyHistogram`, a barrier-synced `WorkloadRunner`, and six shared scenario `Workload`s — driven by two front-ends: a GoogleTest correctness target (CTest label `stress`, fake-only, built under sanitizers) and a standalone `halcyon_stress` perf binary (fake or live Db2, scaling sweep + soft gates). No production code changes.

**Tech Stack:** C++17, CMake (≥3.20), GoogleTest (already vendored via FetchContent), `-fsanitize=thread|address|undefined`, `std::thread`/`std::mutex`/`std::condition_variable`/`std::atomic`.

**Spec:** `docs/superpowers/specs/2026-06-21-halcyon-stress-test-design.md`

---

## File Structure

- Create `tests/stress/CMakeLists.txt` — builds both front-ends; applies sanitizer flags; labels the gtest target `stress`.
- Create `tests/stress/support/latency_histogram.hpp` — fixed-bucket log-scale latency histogram, merge + percentiles.
- Create `tests/stress/support/concurrent_fake_driver.hpp` — thread-safe `ICliDriver` with deterministic SQL-derived results, latency knobs, 1-in-N fault injection, atomic counters.
- Create `tests/stress/support/workload_runner.hpp` / `.cpp` — `Stop`/`RunConfig`/`WorkerCtx`/`RunReport`/`Workload` + `run_workload()`.
- Create `tests/stress/support/workloads.hpp` — `ScenarioId`, `config_for()`, `make_*_workload()` factories, the `One` row struct, shared by both front-ends.
- Create `tests/stress/correctness/test_stress_concurrency.cpp` — one `TEST` per scenario + support self-tests.
- Create `tests/stress/perf/halcyon_stress_main.cpp` — CLI, scaling sweep, report, soft gates.
- Create `tests/stress/README.md` — how to build/run.
- Modify `CMakeLists.txt` — add `HALCYON_BUILD_STRESS_TESTS` option (default OFF) and `HALCYON_SANITIZER` string option.
- Modify `tests/CMakeLists.txt` — `add_subdirectory(stress)` when the option is ON.
- Modify `AGENTS.md` — document the stress/perf invocation.

All stress code is in namespace `halcyon::stress` and depends only on public headers plus `halcyon/detail/cli/driver.hpp`.

---

## Task 1: CMake scaffolding + sanitizer wiring + smoke gate

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/stress/CMakeLists.txt`
- Create: `tests/stress/correctness/test_stress_concurrency.cpp`

- [x] **Step 1: Add the CMake options**

In `CMakeLists.txt`, after the existing `option(HALCYON_BUILD_SMOKE_TEST ...)` line (currently line 19), add:

```cmake
option(HALCYON_BUILD_STRESS_TESTS "Build concurrency stress + perf suite" OFF)
set(HALCYON_SANITIZER "" CACHE STRING
    "Sanitizer for stress targets: thread|address|undefined (empty = none)")
set_property(CACHE HALCYON_SANITIZER PROPERTY STRINGS "" thread address undefined)
```

- [x] **Step 2: Wire the subdirectory**

In `tests/CMakeLists.txt`, after the existing `if(HALCYON_BUILD_INTEGRATION_TESTS) ... endif()` block, add:

```cmake
if(HALCYON_BUILD_STRESS_TESTS)
    add_subdirectory(stress)
endif()
```

- [x] **Step 3: Create the stress CMakeLists with a sanitizer helper and the gtest target**

Create `tests/stress/CMakeLists.txt`:

```cmake
# Applies the selected sanitizer (if any) to a target.
function(halcyon_apply_sanitizer target)
    if(HALCYON_SANITIZER)
        target_compile_options(${target} PRIVATE
            -fsanitize=${HALCYON_SANITIZER} -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE -fsanitize=${HALCYON_SANITIZER})
    endif()
endfunction()

# ---- Front-end #1: correctness (GoogleTest, CTest label "stress") ----
add_executable(halcyon_stress_tests
    correctness/test_stress_concurrency.cpp
    support/workload_runner.cpp)
target_link_libraries(halcyon_stress_tests
    PRIVATE halcyon::halcyon GTest::gtest_main Threads::Threads)
target_include_directories(halcyon_stress_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_compile_options(halcyon_stress_tests PRIVATE -Wall -Wextra)
halcyon_apply_sanitizer(halcyon_stress_tests)

include(GoogleTest)
gtest_discover_tests(halcyon_stress_tests PROPERTIES LABELS "stress")

# ---- Front-end #2: perf harness (standalone; not a default CTest test) ----
add_executable(halcyon_stress
    perf/halcyon_stress_main.cpp
    support/workload_runner.cpp)
target_link_libraries(halcyon_stress
    PRIVATE halcyon::halcyon Threads::Threads)
target_include_directories(halcyon_stress
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_compile_options(halcyon_stress PRIVATE -Wall -Wextra)
halcyon_apply_sanitizer(halcyon_stress)
```

- [x] **Step 4: Create a minimal smoke test so both targets compile and link**

Create `tests/stress/correctness/test_stress_concurrency.cpp`:

```cpp
#include <gtest/gtest.h>

TEST(StressSmoke, TargetBuildsAndRuns) {
    EXPECT_TRUE(true);
}
```

Create a temporary `tests/stress/perf/halcyon_stress_main.cpp` (replaced in Task 11) and the runner stubs so both link:

```cpp
int main() { return 0; }
```

Create `tests/stress/support/workload_runner.cpp`:

```cpp
// Definitions added in Task 4.
```

- [x] **Step 5: Configure and build to verify the scaffolding**

Run:

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON -DHALCYON_BUILD_STRESS_TESTS=ON
cmake --build build -j --target halcyon_stress_tests halcyon_stress
```

Expected: both targets build. `ctest --test-dir build -L stress -N` lists `StressSmoke.TargetBuildsAndRuns`.

- [x] **Step 6: Run the labeled test**

Run: `ctest --test-dir build -L stress --output-on-failure`
Expected: 1 test, PASS.

- [x] **Step 7: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt tests/stress/
git commit -m "build: scaffold concurrency stress suite (targets, sanitizer option, stress label)"
```

---

## Task 2: `LatencyHistogram`

**Files:**
- Create: `tests/stress/support/latency_histogram.hpp`
- Test: `tests/stress/correctness/test_stress_concurrency.cpp`

- [x] **Step 1: Write the failing test**

Replace the body of `tests/stress/correctness/test_stress_concurrency.cpp` with:

```cpp
#include <gtest/gtest.h>

#include "latency_histogram.hpp"

using halcyon::stress::LatencyHistogram;

TEST(LatencyHistogramTest, CountsAndMerges) {
    LatencyHistogram a;
    for (int i = 0; i < 100; ++i) a.record(1000);  // 1us each
    LatencyHistogram b;
    for (int i = 0; i < 100; ++i) b.record(1000);
    a.merge(b);
    EXPECT_EQ(a.count(), 200u);
}

TEST(LatencyHistogramTest, PercentileIsMonotonic) {
    LatencyHistogram h;
    for (int i = 0; i < 1000; ++i) h.record(1000);          // ~1us
    for (int i = 0; i < 10; ++i) h.record(1000ull * 1000);  // ~1ms tail
    EXPECT_GT(h.count(), 0u);
    EXPECT_LE(h.percentile_us(0.50), h.percentile_us(0.99));
    EXPECT_LE(h.percentile_us(0.99), h.max_us());
}
```

- [x] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target halcyon_stress_tests`
Expected: FAIL to compile — `latency_histogram.hpp` not found.

- [x] **Step 3: Implement the histogram**

Create `tests/stress/support/latency_histogram.hpp`:

```cpp
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace halcyon::stress {

// Fixed-bucket log2 histogram of nanosecond latencies. Bucket i holds samples in
// [2^i, 2^(i+1)) ns. Allocation-free, trivially mergeable; percentiles return the
// bucket's upper bound in microseconds (approximate by design — cheap and
// distortion-free on the hot path).
class LatencyHistogram {
public:
    static constexpr int kBuckets = 64;

    void record(std::uint64_t ns) {
        ++counts_[bucket_for(ns)];
        ++total_;
    }

    void merge(const LatencyHistogram& o) {
        for (int i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        total_ += o.total_;
    }

    std::uint64_t count() const { return total_; }

    double percentile_us(double p) const {
        if (total_ == 0) return 0.0;
        const std::uint64_t target =
            static_cast<std::uint64_t>(p * static_cast<double>(total_));
        std::uint64_t cum = 0;
        for (int i = 0; i < kBuckets; ++i) {
            cum += counts_[i];
            if (cum >= target) return bucket_upper_us(i);
        }
        return max_us();
    }

    double max_us() const {
        for (int i = kBuckets - 1; i >= 0; --i)
            if (counts_[i]) return bucket_upper_us(i);
        return 0.0;
    }

private:
    static int bucket_for(std::uint64_t ns) {
        int b = 0;
        while ((ns >>= 1) && b < kBuckets - 1) ++b;
        return b;
    }
    static double bucket_upper_us(int i) {
        // upper bound of bucket i is 2^(i+1) ns -> /1000 us. Use ldexp so the
        // top bucket (i+1 == 64) never triggers shift-by-64 UB.
        return std::ldexp(1.0, i + 1) / 1000.0;
    }

    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t total_ = 0;
};

}  // namespace halcyon::stress
```

- [x] **Step 4: Run to verify it passes**

Run: `cmake --build build -j --target halcyon_stress_tests && ctest --test-dir build -L stress --output-on-failure -R LatencyHistogramTest`
Expected: 2 tests PASS.

- [x] **Step 5: Commit**

```bash
git add tests/stress/support/latency_histogram.hpp tests/stress/correctness/test_stress_concurrency.cpp
git commit -m "test: add LatencyHistogram for the stress suite"
```

---

## Task 3: `ConcurrentFakeDriver`

**Files:**
- Create: `tests/stress/support/concurrent_fake_driver.hpp`
- Test: `tests/stress/correctness/test_stress_concurrency.cpp`

- [x] **Step 1: Write the failing self-tests**

Append to `tests/stress/correctness/test_stress_concurrency.cpp` (keep the histogram tests and the `#include`s above):

```cpp
#include "concurrent_fake_driver.hpp"

using halcyon::stress::ConcurrentFakeDriver;
namespace cli = halcyon::detail::cli;

TEST(FakeDriverTest, SelectReturnsSqlEncodedValue) {
    ConcurrentFakeDriver d;
    auto c = d.connect({"dsn"});
    ASSERT_TRUE(c.ok());
    auto s = d.prepare(c.value(), "SELECT 7 FROM SYSIBM.SYSDUMMY1");
    ASSERT_TRUE(s.ok());
    ASSERT_TRUE(d.bindParams(s.value(), {}).ok());
    ASSERT_TRUE(d.execute(s.value()).ok());
    EXPECT_EQ(d.columnCount(s.value()).value(), 1u);
    ASSERT_TRUE(d.fetch(s.value()).value());  // one row
    auto col = d.getColumn(s.value(), 0);
    ASSERT_TRUE(col.ok());
    EXPECT_EQ(std::get<std::int64_t>(col.value()), 7);
    EXPECT_FALSE(d.fetch(s.value()).value());  // end of cursor
}

TEST(FakeDriverTest, FailExecuteEveryTripsAtRate) {
    ConcurrentFakeDriver d;
    d.failExecuteEvery = 2;  // every 2nd execute fails, retriable
    auto c = d.connect({"dsn"}).value();
    auto s = d.prepare(c, "SELECT 1 FROM SYSIBM.SYSDUMMY1").value();
    EXPECT_TRUE(d.execute(s).ok());        // call 1
    auto r2 = d.execute(s);                // call 2 -> fail
    ASSERT_FALSE(r2.ok());
    EXPECT_TRUE(r2.error().retriable);
}

TEST(FakeDriverTest, StatementOnDeadConnectionErrors) {
    ConcurrentFakeDriver d;
    auto c = d.connect({"dsn"}).value();
    auto s = d.prepare(c, "SELECT 1 FROM SYSIBM.SYSDUMMY1").value();
    ASSERT_TRUE(d.disconnect(c).ok());
    EXPECT_FALSE(d.execute(s).ok());  // use after the connection died
}
```

- [x] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target halcyon_stress_tests`
Expected: FAIL to compile — `concurrent_fake_driver.hpp` not found.

- [x] **Step 3: Implement the fake driver**

Create `tests/stress/support/concurrent_fake_driver.hpp`:

```cpp
#pragma once

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"

namespace halcyon::stress {

// A thread-safe ICliDriver for concurrency testing. Unlike MockCliDriver's FIFO
// scripting, every response is derived from its inputs, so any number of threads
// may call it with no ordering assumptions. Counters are atomic; the handle maps
// are guarded by one mutex (the fake is intentionally a little pessimistic — the
// real contention we measure lives in Halcyon's pool/executor, not here).
class ConcurrentFakeDriver final : public detail::cli::ICliDriver {
public:
    using ConnectionHandle = detail::cli::ConnectionHandle;
    using StatementHandle = detail::cli::StatementHandle;
    using ConnectionParams = detail::cli::ConnectionParams;
    using Value = detail::cli::Value;

    // --- realism knobs (microseconds; 0 = no sleep) ---
    std::atomic<long> queryLatencyUs{0};
    std::atomic<long> connectLatencyUs{0};

    // --- fault injection (1-in-N; 0 disables) ---
    std::atomic<int> failConnectEvery{0};
    std::atomic<int> failExecuteEvery{0};
    std::atomic<int> killConnectionEvery{0};

    // --- counters ---
    std::atomic<long> connectCalls{0};
    std::atomic<long> disconnectCalls{0};
    std::atomic<long> prepareCalls{0};
    std::atomic<long> executeCalls{0};
    std::atomic<long> closeCursorCalls{0};
    std::atomic<long> aliveCalls{0};
    std::atomic<long> finalizeCalls{0};
    std::atomic<long> commitCalls{0};
    std::atomic<long> rollbackCalls{0};
    std::atomic<long> autoCommitOff{0};
    std::atomic<long> autoCommitOn{0};
    std::atomic<long> inFlight{0};
    std::atomic<long> peakInFlight{0};

    // --- Connection lifecycle ---
    Result<ConnectionHandle> connect(const ConnectionParams&) override {
        const long n = ++connectCalls;
        sleep_us(connectLatencyUs.load());
        const int every = failConnectEvery.load();
        if (every > 0 && n % every == 0) return transient<ConnectionHandle>();
        const auto h = static_cast<ConnectionHandle>(next_conn_.fetch_add(1) + 1);
        std::lock_guard<std::mutex> lk(mu_);
        live_.insert(h);
        return Result<ConnectionHandle>(h);
    }

    Result<void> disconnect(ConnectionHandle h) override {
        ++disconnectCalls;
        std::lock_guard<std::mutex> lk(mu_);
        live_.erase(h);
        dead_.erase(h);
        return Result<void>();
    }

    Result<bool> isAlive(ConnectionHandle h) override {
        ++aliveCalls;
        std::lock_guard<std::mutex> lk(mu_);
        if (dead_.count(h)) return Result<bool>(false);
        return Result<bool>(live_.count(h) != 0);
    }

    // --- Prepared-statement path ---
    Result<StatementHandle> prepare(ConnectionHandle conn,
                                    const std::string& sql) override {
        ++prepareCalls;
        std::lock_guard<std::mutex> lk(mu_);
        if (!live_.count(conn)) return stale<StatementHandle>();
        const auto h = static_cast<StatementHandle>(next_stmt_.fetch_add(1) + 1);
        stmts_[h] = StmtState{conn, sql, -1};
        return Result<StatementHandle>(h);
    }

    Result<void> bindParams(StatementHandle stmt,
                            const std::vector<Value>&) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = stmts_.find(stmt);
        if (it == stmts_.end() || !live_.count(it->second.conn))
            return stale<void>();
        return Result<void>();
    }

    Result<std::int64_t> execute(StatementHandle stmt) override {
        const long n = ++executeCalls;
        enter_in_flight();
        sleep_us(queryLatencyUs.load());
        InFlightGuard g{this};

        const int fe = failExecuteEvery.load();
        if (fe > 0 && n % fe == 0) return transient<std::int64_t>();

        std::lock_guard<std::mutex> lk(mu_);
        auto it = stmts_.find(stmt);
        if (it == stmts_.end() || !live_.count(it->second.conn))
            return stale<std::int64_t>();
        it->second.position = -1;
        const int ke = killConnectionEvery.load();
        if (ke > 0 && n % ke == 0) {
            live_.erase(it->second.conn);   // next isAlive -> false
            dead_.insert(it->second.conn);
        }
        return Result<std::int64_t>(is_select(it->second.sql) ? 0 : 1);
    }

    Result<std::size_t> columnCount(StatementHandle stmt) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = stmts_.find(stmt);
        if (it == stmts_.end()) return Result<std::size_t>(std::size_t{0});
        return Result<std::size_t>(is_select(it->second.sql) ? std::size_t{1}
                                                             : std::size_t{0});
    }

    Result<std::string> columnName(StatementHandle, std::size_t) override {
        return Result<std::string>(std::string{"c0"});
    }

    Result<bool> fetch(StatementHandle stmt) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = stmts_.find(stmt);
        if (it == stmts_.end() || !live_.count(it->second.conn))
            return stale<bool>();
        ++it->second.position;
        return Result<bool>(it->second.position < 1);  // exactly one row
    }

    Result<Value> getColumn(StatementHandle stmt, std::size_t index) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = stmts_.find(stmt);
        if (it == stmts_.end() || it->second.position != 0 || index != 0)
            return mapping<Value>();
        return Result<Value>(Value{encoded_value(it->second.sql)});
    }

    Result<void> finalize(StatementHandle stmt) override {
        ++finalizeCalls;
        std::lock_guard<std::mutex> lk(mu_);
        stmts_.erase(stmt);
        return Result<void>();
    }

    Result<void> closeCursor(StatementHandle stmt) override {
        ++closeCursorCalls;
        std::lock_guard<std::mutex> lk(mu_);
        auto it = stmts_.find(stmt);
        if (it != stmts_.end()) it->second.position = -1;
        return Result<void>();
    }

    // --- Transaction control ---
    Result<void> setAutoCommit(ConnectionHandle, bool enabled) override {
        if (enabled) ++autoCommitOn; else ++autoCommitOff;
        return Result<void>();
    }
    Result<void> commit(ConnectionHandle) override {
        ++commitCalls;
        return Result<void>();
    }
    Result<void> rollback(ConnectionHandle) override {
        ++rollbackCalls;
        return Result<void>();
    }

private:
    struct StmtState {
        ConnectionHandle conn;
        std::string sql;
        long position;
    };
    struct InFlightGuard {
        ConcurrentFakeDriver* d;
        ~InFlightGuard() { --d->inFlight; }
    };

    void enter_in_flight() {
        const long cur = ++inFlight;
        long prev = peakInFlight.load();
        while (cur > prev && !peakInFlight.compare_exchange_weak(prev, cur)) {}
    }

    static void sleep_us(long us) {
        if (us > 0) std::this_thread::sleep_for(std::chrono::microseconds(us));
    }

    // Leading non-space token is SELECT or VALUES (the fake never sees real DML
    // result sets; DML returns an affected-row count instead).
    static bool is_select(const std::string& sql) {
        std::size_t i = 0;
        while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i])))
            ++i;
        auto starts = [&](const char* kw) {
            std::size_t k = 0;
            while (kw[k] && i + k < sql.size() &&
                   std::toupper(static_cast<unsigned char>(sql[i + k])) == kw[k])
                ++k;
            return kw[k] == '\0';
        };
        return starts("SELECT") || starts("VALUES");
    }

    // The first contiguous run of digits in the SQL (e.g. "SELECT 7 FROM ..." ->
    // 7). Both fake and live backends run "SELECT <n> FROM SYSIBM.SYSDUMMY1", so a
    // workload always knows the expected scalar. No digits -> 0.
    static std::int64_t encoded_value(const std::string& sql) {
        std::int64_t v = 0;
        bool in = false;
        for (char ch : sql) {
            if (ch >= '0' && ch <= '9') {
                v = v * 10 + (ch - '0');
                in = true;
            } else if (in) {
                break;
            }
        }
        return v;
    }

    template <class T>
    static Result<T> transient() {
        Error e;
        e.code = ErrorCode::Transient;
        e.sqlstate = "40001";
        e.message = "injected transient";
        e.retriable = true;
        return Result<T>(e);
    }
    template <class T>
    static Result<T> stale() {
        Error e;
        e.code = ErrorCode::Connection;
        e.sqlstate = "08003";
        e.message = "statement used on a dead connection";
        e.retriable = true;
        return Result<T>(e);
    }
    template <class T>
    static Result<T> mapping() {
        Error e;
        e.code = ErrorCode::Mapping;
        e.message = "column index out of range";
        return Result<T>(e);
    }

    std::mutex mu_;
    std::unordered_set<ConnectionHandle> live_;
    std::unordered_set<ConnectionHandle> dead_;
    std::unordered_map<StatementHandle, StmtState> stmts_;
    std::atomic<std::uint64_t> next_conn_{0};
    std::atomic<std::uint64_t> next_stmt_{0};
};

}  // namespace halcyon::stress
```

- [x] **Step 4: Run to verify it passes**

Run: `cmake --build build -j --target halcyon_stress_tests && ctest --test-dir build -L stress --output-on-failure -R FakeDriverTest`
Expected: 3 tests PASS.

- [x] **Step 5: Commit**

```bash
git add tests/stress/support/concurrent_fake_driver.hpp tests/stress/correctness/test_stress_concurrency.cpp
git commit -m "test: add thread-safe ConcurrentFakeDriver for stress suite"
```

---

## Task 4: `WorkloadRunner`

**Files:**
- Create: `tests/stress/support/workload_runner.hpp`
- Modify: `tests/stress/support/workload_runner.cpp`
- Test: `tests/stress/correctness/test_stress_concurrency.cpp`

- [x] **Step 1: Write the failing tests**

Append to `tests/stress/correctness/test_stress_concurrency.cpp`:

```cpp
#include <atomic>

#include "workload_runner.hpp"

using halcyon::stress::RunConfig;
using halcyon::stress::RunReport;
using halcyon::stress::Stop;
using halcyon::stress::WorkerCtx;
using halcyon::stress::Workload;
using halcyon::stress::run_workload;

TEST(WorkloadRunnerTest, RunsExactlyTotalIterationsAcrossThreads) {
    std::atomic<long> ops{0};
    Workload w{"count", [&](WorkerCtx&) { ++ops; }};
    RunConfig cfg;
    cfg.threads = 4;
    cfg.stop.total_iters = 4000;
    RunReport r = run_workload(w, cfg);
    EXPECT_EQ(ops.load(), 4000);
    EXPECT_EQ(r.ops, 4000u);
    EXPECT_FALSE(r.failed);
    EXPECT_EQ(r.hist.count(), 4000u);
}

TEST(WorkloadRunnerTest, SurfacesWorkerFailure) {
    Workload w{"boom", [](WorkerCtx& ctx) { ctx.fail("kaboom"); }};
    RunConfig cfg;
    cfg.threads = 2;
    cfg.stop.total_iters = 10;
    RunReport r = run_workload(w, cfg);
    EXPECT_TRUE(r.failed);
    EXPECT_EQ(r.first_error, "kaboom");
}

TEST(WorkloadRunnerTest, DurationModeRunsAndReportsThroughput) {
    Workload w{"spin", [](WorkerCtx&) {}};
    RunConfig cfg;
    cfg.threads = 2;
    cfg.stop.duration = std::chrono::milliseconds(50);
    RunReport r = run_workload(w, cfg);
    EXPECT_GT(r.ops, 0u);
    EXPECT_GT(r.throughput_per_sec(), 0.0);
}
```

- [x] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target halcyon_stress_tests`
Expected: FAIL to compile — `workload_runner.hpp` not found.

- [x] **Step 3: Declare the runner types**

Create `tests/stress/support/workload_runner.hpp`:

```cpp
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
```

- [x] **Step 4: Implement the runner**

Replace `tests/stress/support/workload_runner.cpp` with:

```cpp
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
```

- [x] **Step 5: Run to verify it passes**

Run: `cmake --build build -j --target halcyon_stress_tests && ctest --test-dir build -L stress --output-on-failure -R WorkloadRunnerTest`
Expected: 3 tests PASS. (Note: in `total_iters` mode the exact op count holds because the shared countdown is decremented atomically; a worker that draws `<= 0` exits without running the body.)

- [x] **Step 6: Commit**

```bash
git add tests/stress/support/workload_runner.hpp tests/stress/support/workload_runner.cpp tests/stress/correctness/test_stress_concurrency.cpp
git commit -m "test: add barrier-synced WorkloadRunner with latency capture"
```

---

## Task 5: Shared scenario factories (`workloads.hpp`) + Scenario 1 (pool contention)

**Files:**
- Create: `tests/stress/support/workloads.hpp`
- Test: `tests/stress/correctness/test_stress_concurrency.cpp`

- [x] **Step 1: Write the failing test**

Append to `tests/stress/correctness/test_stress_concurrency.cpp`:

```cpp
#include <memory>

#include "halcyon/database.hpp"
#include "workloads.hpp"

using halcyon::Database;
using halcyon::stress::config_for;
using halcyon::stress::make_pool_contention;
using halcyon::stress::ScenarioId;

TEST(StressScenario, PoolContentionStaysConsistent) {
    auto fake = std::make_shared<ConcurrentFakeDriver>();
    auto db = Database::open(fake, "dsn", config_for(ScenarioId::Pool, 8));
    ASSERT_TRUE(db.ok()) << db.error().message;

    Workload w = make_pool_contention(db.value());
    RunConfig cfg;
    cfg.threads = 32;                 // threads >> pool max
    cfg.stop.total_iters = 20000;
    RunReport r = run_workload(w, cfg);

    EXPECT_FALSE(r.failed) << r.first_error;
    EXPECT_LE(fake->peakInFlight.load(), 8);  // never more leases than max
    EXPECT_EQ(db.value().pool().active_count(), 0u);
    EXPECT_EQ(db.value().pool().idle_count(), db.value().pool().total_count());
}
```

- [x] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target halcyon_stress_tests`
Expected: FAIL to compile — `workloads.hpp` not found.

- [x] **Step 3: Implement `workloads.hpp` with `config_for`, the `One` row, and the pool-contention factory**

Create `tests/stress/support/workloads.hpp`:

```cpp
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "halcyon/database.hpp"
#include "halcyon/pool.hpp"
#include "halcyon/types.hpp"
#include "concurrent_fake_driver.hpp"
#include "workload_runner.hpp"

namespace halcyon::stress {

// Single-column scalar row used by every scalar SELECT in the workloads. The fake
// (and live Db2) return one int64 column equal to the integer embedded in the SQL.
struct One {
    std::int64_t v;
};

}  // namespace halcyon::stress

HALCYON_REFLECT(halcyon::stress::One, v);

namespace halcyon::stress {

enum class ScenarioId { Pool, Executor, Cache, Reconnect, Txn, Lifecycle };

// The tuned PoolConfig for each scenario. pool_max is the only sweepable knob the
// perf harness varies; everything else is scenario-intrinsic.
inline PoolConfig config_for(ScenarioId id, std::size_t pool_max) {
    using namespace std::chrono_literals;
    PoolConfig cfg;
    cfg.max = pool_max == 0 ? 1 : pool_max;
    cfg.min = cfg.max < 2 ? cfg.max : 2;
    cfg.startMaintenanceThread = false;
    cfg.statementCacheSize = 0;
    cfg.backoff.sleep = [](std::chrono::milliseconds) {};  // never really wait
    switch (id) {
        case ScenarioId::Pool:
            cfg.acquireTimeout = 50ms;
            break;
        case ScenarioId::Executor:
            cfg.statementCacheSize = 16;
            break;
        case ScenarioId::Cache:
            cfg.min = 1;
            cfg.max = 2;
            cfg.statementCacheSize = 8;
            break;
        case ScenarioId::Reconnect:
            cfg.validateOnAcquire = true;
            cfg.backoff.maxAttempts = 3;
            cfg.statementCacheSize = 8;
            break;
        case ScenarioId::Txn:
            break;
        case ScenarioId::Lifecycle:
            cfg.startMaintenanceThread = true;
            cfg.maintenanceInterval = 1ms;
            cfg.idleTimeout = 1ms;
            cfg.maxLifetime = 5ms;
            break;
    }
    return cfg;
}

// A canonical scalar SELECT whose result the fake/live encode as `n`.
inline std::string select_n(std::int64_t n) {
    return "SELECT " + std::to_string(n) + " FROM SYSIBM.SYSDUMMY1";
}

// --- Scenario 1: raw pool acquire/release contention (pool mechanics only) ---
// Acquire a pooled connection, run one scalar query directly on it (no facade
// retry), release via RAII scope. A pool-timeout error is expected under
// contention and is NOT a failure.
inline Workload make_pool_contention(Database& db) {
    return Workload{"pool", [&db](WorkerCtx& ctx) {
        auto pc = db.pool().acquire();
        if (!pc.ok()) {
            if (pc.error().code != ErrorCode::Pool)
                ctx.fail("unexpected acquire error: " + pc.error().message);
            return;  // timeout under contention is acceptable
        }
        auto r = pc.value()->queryAs<One>(select_n(7));
        if (!r.ok()) {
            ctx.fail("query failed: " + r.error().message);
            return;
        }
        if (r.value().size() != 1 || r.value()[0].v != 7)
            ctx.fail("wrong scalar from pooled query");
    }};
}

}  // namespace halcyon::stress
```

- [x] **Step 4: Run to verify it passes**

Run: `cmake --build build -j --target halcyon_stress_tests && ctest --test-dir build -L stress --output-on-failure -R StressScenario.PoolContention`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add tests/stress/support/workloads.hpp tests/stress/correctness/test_stress_concurrency.cpp
git commit -m "test: add shared scenario factories + pool-contention stress scenario"
```

---

## Task 6: Scenario 2 (async executor saturation)

**Files:**
- Modify: `tests/stress/support/workloads.hpp`
- Test: `tests/stress/correctness/test_stress_concurrency.cpp`

- [x] **Step 1: Write the failing test**

Append to `tests/stress/correctness/test_stress_concurrency.cpp`:

```cpp
using halcyon::stress::make_executor_saturation;

TEST(StressScenario, ExecutorSaturationResolvesEveryFuture) {
    auto fake = std::make_shared<ConcurrentFakeDriver>();
    auto db = Database::open(fake, "dsn", config_for(ScenarioId::Executor, 4));
    ASSERT_TRUE(db.ok()) << db.error().message;

    Workload w = make_executor_saturation(db.value());
    RunConfig cfg;
    cfg.threads = 16;                 // far more launchers than executor threads
    cfg.stop.total_iters = 8000;
    RunReport r = run_workload(w, cfg);

    EXPECT_FALSE(r.failed) << r.first_error;
    EXPECT_EQ(r.ops, 8000u);          // every op (a launched+awaited future) done
    EXPECT_EQ(db.value().pool().active_count(), 0u);
}
```

- [x] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target halcyon_stress_tests`
Expected: FAIL to compile — `make_executor_saturation` undeclared.

- [x] **Step 3: Add the factory**

In `tests/stress/support/workloads.hpp`, add before the closing `}  // namespace halcyon::stress`:

```cpp
// --- Scenario 2: async executor saturation (product path: Database async) ---
// Each op launches a typed async query on the Database's internal executor and
// blocks on its future. Many runner threads launching at once saturate the fixed
// executor; every future must resolve exactly once with the right scalar.
inline Workload make_executor_saturation(Database& db) {
    return Workload{"executor", [&db](WorkerCtx& ctx) {
        auto fut = db.queryAsync<One>(select_n(7));
        auto r = fut.get();
        if (!r.ok()) {
            ctx.fail("async query failed: " + r.error().message);
            return;
        }
        if (r.value().size() != 1 || r.value()[0].v != 7)
            ctx.fail("wrong scalar from async query");
    }};
}
```

- [x] **Step 4: Run to verify it passes**

Run: `cmake --build build -j --target halcyon_stress_tests && ctest --test-dir build -L stress --output-on-failure -R StressScenario.ExecutorSaturation`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add tests/stress/support/workloads.hpp tests/stress/correctness/test_stress_concurrency.cpp
git commit -m "test: add async executor-saturation stress scenario"
```

---

## Task 7: Scenario 3 (statement cache correctness under reuse)

**Files:**
- Modify: `tests/stress/support/workloads.hpp`
- Test: `tests/stress/correctness/test_stress_concurrency.cpp`

- [x] **Step 1: Write the failing test**

Append to `tests/stress/correctness/test_stress_concurrency.cpp`:

```cpp
using halcyon::stress::make_cache_churn;

TEST(StressScenario, StatementCacheStaysCorrectUnderReuse) {
    auto fake = std::make_shared<ConcurrentFakeDriver>();
    auto db = Database::open(fake, "dsn", config_for(ScenarioId::Cache, 2));
    ASSERT_TRUE(db.ok()) << db.error().message;

    Workload w = make_cache_churn(db.value());
    RunConfig cfg;
    cfg.threads = 8;
    cfg.stop.total_iters = 20000;
    RunReport r = run_workload(w, cfg);

    EXPECT_FALSE(r.failed) << r.first_error;
    // Reuse really happened: far fewer prepares than executes (a warm cache on a
    // reused connection re-prepares only on miss/overflow, not per call).
    EXPECT_LT(fake->prepareCalls.load(), fake->executeCalls.load());
    EXPECT_EQ(db.value().pool().active_count(), 0u);
}
```

- [x] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target halcyon_stress_tests`
Expected: FAIL to compile — `make_cache_churn` undeclared.

- [x] **Step 3: Add the factory**

In `tests/stress/support/workloads.hpp`, add before the closing namespace brace:

```cpp
// --- Scenario 3: statement-cache correctness under reuse ---
// A small rotating set of SQL strings on few connections (cache enabled) so hits,
// the busy->overflow path (same SQL concurrently), and eviction all occur. Each op
// asserts the scalar matches the SQL it sent (proves no cross-thread row mixup).
inline Workload make_cache_churn(Database& db) {
    return Workload{"cache", [&db](WorkerCtx& ctx) {
        std::int64_t n = 1 + static_cast<std::int64_t>(ctx.rng() % 3);  // 1..3
        auto r = db.queryAs<One>(select_n(n));
        if (!r.ok()) {
            ctx.fail("cache query failed: " + r.error().message);
            return;
        }
        if (r.value().size() != 1 || r.value()[0].v != n)
            ctx.fail("wrong scalar from cached query");
    }};
}
```

- [x] **Step 4: Run to verify it passes**

Run: `cmake --build build -j --target halcyon_stress_tests && ctest --test-dir build -L stress --output-on-failure -R StressScenario.StatementCache`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add tests/stress/support/workloads.hpp tests/stress/correctness/test_stress_concurrency.cpp
git commit -m "test: add statement-cache reuse stress scenario"
```

---

## Task 8: Scenario 4 (reconnect + transient-fault injection under load)

**Files:**
- Modify: `tests/stress/support/workloads.hpp`
- Test: `tests/stress/correctness/test_stress_concurrency.cpp`

- [x] **Step 1: Write the failing test**

Append to `tests/stress/correctness/test_stress_concurrency.cpp`:

```cpp
using halcyon::stress::make_reconnect_faults;

TEST(StressScenario, ReconnectAndRetryRecoverUnderFaults) {
    auto fake = std::make_shared<ConcurrentFakeDriver>();
    fake->failExecuteEvery = 7;       // retriable statement errors
    fake->killConnectionEvery = 11;   // forces validate-on-acquire reconnects
    auto db = Database::open(fake, "dsn", config_for(ScenarioId::Reconnect, 6));
    ASSERT_TRUE(db.ok()) << db.error().message;
    const long connects_after_warmup = fake->connectCalls.load();

    Workload w = make_reconnect_faults(db.value());
    RunConfig cfg;
    cfg.threads = 12;
    cfg.stop.total_iters = 30000;
    RunReport r = run_workload(w, cfg);

    EXPECT_FALSE(r.failed) << r.first_error;            // read-only ops recover
    EXPECT_GT(fake->connectCalls.load(),
              connects_after_warmup);                   // reconnects actually fired
    EXPECT_EQ(db.value().pool().active_count(), 0u);    // no leaked leases
}
```

Rationale for "always recovers": the facade gives read-only SELECTs `idempotent(3)`; a retry re-executes, incrementing the fake's global execute counter, and three attempts can never all land on multiples of 7 (multiples of 7 are never consecutive), so each op succeeds within its attempts.

- [x] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target halcyon_stress_tests`
Expected: FAIL to compile — `make_reconnect_faults` undeclared.

- [x] **Step 3: Add the factory**

In `tests/stress/support/workloads.hpp`, add before the closing namespace brace:

```cpp
// --- Scenario 4: reconnect + transient-fault injection under load ---
// Driven through the facade so the safe-retry policy is in play. With fault knobs
// set on the driver and validateOnAcquire on the pool, read-only queries must be
// transparently recovered (retry and/or in-place reconnect). A surfaced error
// means recovery failed.
inline Workload make_reconnect_faults(Database& db) {
    return Workload{"reconnect", [&db](WorkerCtx& ctx) {
        auto r = db.queryAs<One>(select_n(7));
        if (!r.ok()) {
            ctx.fail("query not recovered: " + r.error().message);
            return;
        }
        if (r.value().size() != 1 || r.value()[0].v != 7)
            ctx.fail("wrong scalar after recovery");
    }};
}
```

- [x] **Step 4: Run to verify it passes**

Run: `cmake --build build -j --target halcyon_stress_tests && ctest --test-dir build -L stress --output-on-failure -R StressScenario.ReconnectAndRetry`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add tests/stress/support/workloads.hpp tests/stress/correctness/test_stress_concurrency.cpp
git commit -m "test: add reconnect + fault-injection stress scenario"
```

---

## Task 9: Scenario 5 (transaction churn)

**Files:**
- Modify: `tests/stress/support/workloads.hpp`
- Test: `tests/stress/correctness/test_stress_concurrency.cpp`

- [x] **Step 1: Write the failing test**

Append to `tests/stress/correctness/test_stress_concurrency.cpp`:

```cpp
using halcyon::stress::make_txn_churn;

TEST(StressScenario, TransactionChurnCommitsAndRollbacksBalance) {
    auto fake = std::make_shared<ConcurrentFakeDriver>();
    auto db = Database::open(fake, "dsn", config_for(ScenarioId::Txn, 6));
    ASSERT_TRUE(db.ok()) << db.error().message;

    Workload w = make_txn_churn(db.value());
    RunConfig cfg;
    cfg.threads = 8;
    cfg.stop.total_iters = 16000;
    RunReport r = run_workload(w, cfg);

    EXPECT_FALSE(r.failed) << r.first_error;
    const long txns = fake->autoCommitOff.load();         // each begin() flips off
    EXPECT_EQ(txns, 16000);
    EXPECT_EQ(fake->commitCalls.load() + fake->rollbackCalls.load(), txns);
    EXPECT_EQ(fake->autoCommitOn.load(), txns);           // each tx restores it
    EXPECT_EQ(db.value().pool().active_count(), 0u);      // no tx leaks a connection
}
```

- [x] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target halcyon_stress_tests`
Expected: FAIL to compile — `make_txn_churn` undeclared.

- [x] **Step 3: Add the factory**

In `tests/stress/support/workloads.hpp`, add before the closing namespace brace (it needs `<halcyon/transaction.hpp>` types via the facade, already included through `database.hpp`):

```cpp
// --- Scenario 5: transaction churn (facade transaction(); commit + rollback) ---
// Half the ops commit (lambda returns ok) and half roll back (lambda returns an
// error Result), exercising ScopedTransaction's commit/rollback + lease-return.
inline Workload make_txn_churn(Database& db) {
    return Workload{"txn", [&db](WorkerCtx& ctx) {
        const bool do_commit = (ctx.rng() & 1u) == 0u;
        auto r = db.transaction(
            [&](Transaction& tx) -> Result<std::int64_t> {
                auto q = tx.queryAs<One>(select_n(7));
                if (!q.ok()) return q.error();
                if (q.value().size() != 1 || q.value()[0].v != 7) {
                    Error e;
                    e.code = ErrorCode::Mapping;
                    e.message = "wrong scalar in tx";
                    return e;
                }
                if (do_commit) return std::int64_t{1};
                Error e;          // force a rollback path
                e.code = ErrorCode::Unknown;
                e.message = "intentional rollback";
                return e;
            });
        // A committed tx returns ok; a rolled-back tx returns our forced error.
        // A Mapping error means the in-tx read was wrong -> real failure.
        if (!r.ok() && r.error().code == ErrorCode::Mapping)
            ctx.fail("in-transaction read returned wrong scalar");
    }};
}
```

- [x] **Step 4: Run to verify it passes**

Run: `cmake --build build -j --target halcyon_stress_tests && ctest --test-dir build -L stress --output-on-failure -R StressScenario.TransactionChurn`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add tests/stress/support/workloads.hpp tests/stress/correctness/test_stress_concurrency.cpp
git commit -m "test: add transaction-churn stress scenario"
```

---

## Task 10: Scenario 6 (pool lifecycle races + in-flight teardown)

**Files:**
- Modify: `tests/stress/support/workloads.hpp`
- Test: `tests/stress/correctness/test_stress_concurrency.cpp`

- [x] **Step 1: Write the failing tests**

Append to `tests/stress/correctness/test_stress_concurrency.cpp`:

```cpp
#include <future>
#include <vector>

using halcyon::stress::make_pool_contention;  // reused under reaper pressure

TEST(StressScenario, ReaperRacesAcquireReleaseSafely) {
    auto fake = std::make_shared<ConcurrentFakeDriver>();
    // Lifecycle config: maintenance thread on, tiny intervals so the reaper fires
    // constantly while acquires/releases race it.
    auto db = Database::open(fake, "dsn", config_for(ScenarioId::Lifecycle, 6));
    ASSERT_TRUE(db.ok()) << db.error().message;

    Workload w = make_pool_contention(db.value());  // acquire/query/release loop
    RunConfig cfg;
    cfg.threads = 16;
    cfg.stop.duration = std::chrono::milliseconds(300);  // let the reaper churn
    RunReport r = run_workload(w, cfg);

    EXPECT_FALSE(r.failed) << r.first_error;
    EXPECT_LE(fake->peakInFlight.load(), 6);             // reaper never reaps a lease
    EXPECT_EQ(db.value().pool().active_count(), 0u);
    EXPECT_GE(db.value().pool().total_count(),
              db.value().pool().idle_count());           // accounting consistent
}

TEST(StressScenario, DestroyDatabaseWithInflightAsyncWork) {
    auto fake = std::make_shared<ConcurrentFakeDriver>();
    fake->queryLatencyUs = 200;  // make async work linger so it's truly in flight
    std::vector<std::future<halcyon::Result<std::vector<halcyon::stress::One>>>> futs;
    {
        auto db = Database::open(fake, "dsn", config_for(ScenarioId::Executor, 4));
        ASSERT_TRUE(db.ok()) << db.error().message;
        for (int i = 0; i < 200; ++i)
            futs.push_back(db.value().queryAsync<halcyon::stress::One>(
                halcyon::stress::select_n(7)));
        // db (and its shared executor/pool) go out of scope here with futures live;
        // the shared executor drains in-flight tasks before teardown completes.
    }
    for (auto& f : futs) {
        auto r = f.get();
        ASSERT_TRUE(r.ok()) << r.error().message;
        ASSERT_EQ(r.value().size(), 1u);
        EXPECT_EQ(r.value()[0].v, 7);
    }
}
```

- [x] **Step 2: Run to verify it fails or passes-but-incomplete**

Run: `cmake --build build -j --target halcyon_stress_tests`
Expected: compiles (no new factory needed — Scenario 6 reuses `make_pool_contention` and `queryAsync`). If it fails to build, it is because `select_n` must be visible; it already is via `workloads.hpp`.

- [x] **Step 3: No new implementation required**

Scenario 6 exercises existing code paths (the pool maintenance thread + executor drain) and needs no new factory. If a sanitizer flags a real race here, fix the production code under `include/halcyon/` per the systematic-debugging skill — do not paper over it in the test.

- [x] **Step 4: Run to verify it passes**

Run: `cmake --build build -j --target halcyon_stress_tests && ctest --test-dir build -L stress --output-on-failure -R StressScenario`
Expected: all `StressScenario.*` PASS.

- [x] **Step 5: Commit**

```bash
git add tests/stress/correctness/test_stress_concurrency.cpp
git commit -m "test: add pool-lifecycle race + in-flight teardown stress scenarios"
```

---

## Task 11: Perf harness (`halcyon_stress`)

**Files:**
- Modify: `tests/stress/perf/halcyon_stress_main.cpp`
- Test: manual run (not a CTest test)

- [x] **Step 1: Implement the harness**

Replace `tests/stress/perf/halcyon_stress_main.cpp` with:

```cpp
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "halcyon/database.hpp"
#include "halcyon/detail/cli/db2_cli_driver.hpp"
#include "concurrent_fake_driver.hpp"
#include "workloads.hpp"
#include "workload_runner.hpp"

using namespace halcyon;
using namespace halcyon::stress;

namespace {

struct Options {
    std::string backend = "fake";
    std::string scenario = "all";
    std::vector<std::size_t> threads = {1, 2, 4, 8, 16};
    std::size_t pool_max = 8;
    std::chrono::milliseconds duration{5000};
    std::chrono::milliseconds warmup{1000};
    long latency_us = 0;
    std::uint64_t seed = 0;
    std::string format = "table";
    bool strict = false;
};

std::vector<std::size_t> parse_threads(const std::string& csv) {
    std::vector<std::size_t> out;
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ','))
        if (!tok.empty()) out.push_back(static_cast<std::size_t>(std::stoul(tok)));
    return out;
}

bool arg_val(const std::string& a, const char* key, std::string& out) {
    const std::string prefix = std::string(key) + "=";
    if (a.rfind(prefix, 0) == 0) { out = a.substr(prefix.size()); return true; }
    return false;
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i], v;
        if (arg_val(a, "--backend", v)) o.backend = v;
        else if (arg_val(a, "--scenario", v)) o.scenario = v;
        else if (arg_val(a, "--threads", v)) o.threads = parse_threads(v);
        else if (arg_val(a, "--pool-max", v)) o.pool_max = std::stoul(v);
        else if (arg_val(a, "--duration", v)) o.duration = std::chrono::milliseconds(std::stol(v));
        else if (arg_val(a, "--warmup", v)) o.warmup = std::chrono::milliseconds(std::stol(v));
        else if (arg_val(a, "--latency", v)) o.latency_us = std::stol(v);
        else if (arg_val(a, "--seed", v)) o.seed = std::stoull(v);
        else if (arg_val(a, "--format", v)) o.format = v;
        else if (a == "--strict") o.strict = true;
        else { std::cerr << "unknown arg: " << a << "\n"; std::exit(2); }
    }
    return o;
}

struct ScenarioSpec { ScenarioId id; const char* name; };
const std::vector<ScenarioSpec> kScenarios = {
    {ScenarioId::Pool, "pool"},   {ScenarioId::Executor, "executor"},
    {ScenarioId::Cache, "cache"}, {ScenarioId::Reconnect, "reconnect"},
    {ScenarioId::Txn, "txn"},     {ScenarioId::Lifecycle, "lifecycle"},
};

Workload make_workload(ScenarioId id, Database& db) {
    switch (id) {
        case ScenarioId::Pool: return make_pool_contention(db);
        case ScenarioId::Executor: return make_executor_saturation(db);
        case ScenarioId::Cache: return make_cache_churn(db);
        case ScenarioId::Reconnect: return make_reconnect_faults(db);
        case ScenarioId::Txn: return make_txn_churn(db);
        case ScenarioId::Lifecycle: return make_pool_contention(db);
    }
    return make_pool_contention(db);
}

}  // namespace

int main(int argc, char** argv) {
    Options o = parse(argc, argv);

    std::optional<std::string> dsn;
    if (o.backend == "live") {
        const char* env = std::getenv("HALCYON_TEST_DSN");
        if (!env) {
            std::cerr << "--backend=live requires HALCYON_TEST_DSN to be set\n";
            return 2;
        }
        dsn = env;
    } else if (o.backend != "fake") {
        std::cerr << "--backend must be fake or live\n";
        return 2;
    }

    const bool csv = (o.format == "csv");
    std::cout << (csv ? "scenario,threads,pool_max,throughput_ops_s,p50_us,p95_us,"
                        "p99_us,max_us,ops,errors\n"
                      : "scenario        threads pool  thr(ops/s)   p50us   p99us"
                        "   maxus      ops\n");

    int exit_code = 0;
    for (const auto& sc : kScenarios) {
        if (o.scenario != "all" && o.scenario != sc.name) continue;
        // Reconnect's fault injection is fake-only; skip it on the live backend.
        if (o.backend == "live" && sc.id == ScenarioId::Reconnect) continue;

        double thr_at_min = 0.0, thr_peak = 0.0;
        for (std::size_t t : o.threads) {
            // Build a fresh backend per cell so counters/caches start clean.
            std::shared_ptr<ConcurrentFakeDriver> fake;
            Result<Database> dbr = [&] {
                PoolConfig cfg = config_for(sc.id, o.pool_max);
                if (o.backend == "live")
                    return Database::open(*dsn, cfg);
                fake = std::make_shared<ConcurrentFakeDriver>();
                fake->queryLatencyUs = o.latency_us;
                if (sc.id == ScenarioId::Reconnect) {
                    fake->failExecuteEvery = 7;
                    fake->killConnectionEvery = 11;
                }
                return Database::open(
                    std::static_pointer_cast<detail::cli::ICliDriver>(fake),
                    "dsn", cfg);
            }();
            if (!dbr.ok()) {
                std::cerr << "open failed for " << sc.name << ": "
                          << dbr.error().message << "\n";
                exit_code = 1;
                continue;
            }
            Database& db = dbr.value();

            Workload w = make_workload(sc.id, db);
            RunConfig cfg;
            cfg.threads = t;
            cfg.stop.duration = o.duration;
            cfg.seed = o.seed;
            RunReport r = run_workload(w, cfg);

            const double thr = r.throughput_per_sec();
            if (t == o.threads.front()) thr_at_min = thr;
            if (thr > thr_peak) thr_peak = thr;

            if (csv) {
                std::cout << sc.name << ',' << t << ',' << o.pool_max << ','
                          << thr << ',' << r.p50_us() << ',' << r.p95_us() << ','
                          << r.p99_us() << ',' << r.max_us() << ',' << r.ops << ','
                          << (r.failed ? 1 : 0) << '\n';
            } else {
                char line[256];
                std::snprintf(line, sizeof(line),
                              "%-15s %7zu %4zu %12.1f %7.1f %7.1f %8.1f %9llu",
                              sc.name, t, o.pool_max, thr, r.p50_us(), r.p99_us(),
                              r.max_us(),
                              static_cast<unsigned long long>(r.ops));
                std::cout << line << (r.failed ? "  FAILED" : "") << "\n";
            }
            if (r.failed) exit_code = 1;
        }

        // Soft gate (fake backend only): scaling. Reported as PASS/WARN; affects
        // the exit code only under --strict.
        if (o.backend == "fake" && o.threads.size() > 1) {
            const bool scales = thr_peak >= 1.5 * thr_at_min;
            std::cout << "  [gate] " << sc.name << " scaling "
                      << (scales ? "PASS" : "WARN") << " (peak " << thr_peak
                      << " vs base " << thr_at_min << " ops/s)\n";
            if (!scales && o.strict) exit_code = 1;
        }
    }
    return exit_code;
}
```

- [x] **Step 2: Build**

Run: `cmake --build build -j --target halcyon_stress`
Expected: builds clean.

- [x] **Step 3: Run a quick fake sweep to verify output**

Run: `./build/tests/stress/halcyon_stress --scenario=pool --threads=1,2,4,8 --duration=500`
Expected: a table with one row per thread count, throughput rising with threads, and a `[gate] pool scaling PASS|WARN` line. Exit code 0 (no `--strict`).

- [x] **Step 4: Commit**

```bash
git add tests/stress/perf/halcyon_stress_main.cpp
git commit -m "feat: add halcyon_stress perf harness (scaling sweep, report, soft gates)"
```

---

## Task 12: Docs + full sanitizer verification

**Files:**
- Create: `tests/stress/README.md`
- Modify: `AGENTS.md`

- [x] **Step 1: Write the stress README**

Create `tests/stress/README.md`:

```markdown
# Halcyon concurrency stress & performance suite

Two front-ends over one shared core (`support/`):

- **`halcyon_stress_tests`** — GoogleTest correctness suite (CTest label `stress`).
  Runs against the in-process `ConcurrentFakeDriver` (no DB). Build it under a
  sanitizer to prove race/deadlock freedom.
- **`halcyon_stress`** — standalone perf harness. Reports throughput / latency /
  scaling for each scenario against the fake or a live Db2 (`HALCYON_TEST_DSN`).

## Build

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON \
      -DHALCYON_BUILD_STRESS_TESTS=ON -DHALCYON_SANITIZER=thread
cmake --build build -j
```

`HALCYON_SANITIZER` accepts `thread`, `address`, `undefined`, or empty (none).

## Correctness (sanitizer-clean)

```bash
ctest --test-dir build -L stress --output-on-failure
```

A clean run under `-DHALCYON_SANITIZER=thread` is the evidence of race freedom.

## Performance

```bash
# fake backend (pure library overhead), scaling sweep
./build/tests/stress/halcyon_stress --scenario=all --threads=1,2,4,8,16

# live Db2 (report-only)
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
./build/tests/stress/halcyon_stress --backend=live --scenario=pool --threads=4,8,16

# CSV for plotting; --strict makes soft gates set the exit code
./build/tests/stress/halcyon_stress --format=csv --strict > sweep.csv
```
```

- [x] **Step 2: Document in AGENTS.md**

In `AGENTS.md`, under the `## Build & test` section, after the "Key CMake options" paragraph, add:

```markdown
### Concurrency stress & performance suite (opt-in)

Build with `-DHALCYON_BUILD_STRESS_TESTS=ON`. The correctness suite is a CTest
target labeled `stress`; build it under a sanitizer and run it to verify
race/deadlock freedom:

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON \
      -DHALCYON_BUILD_STRESS_TESTS=ON -DHALCYON_SANITIZER=thread
cmake --build build -j
ctest --test-dir build -L stress --output-on-failure
./build/tests/stress/halcyon_stress --scenario=all --threads=1,2,4,8,16   # perf report
```

See `tests/stress/README.md` for details.
```

- [x] **Step 3: Full build + run under ThreadSanitizer**

Run:

```bash
cmake -S . -B build-tsan -DHALCYON_BUILD_TESTS=ON \
      -DHALCYON_BUILD_STRESS_TESTS=ON -DHALCYON_SANITIZER=thread
cmake --build build-tsan -j --target halcyon_stress_tests
ctest --test-dir build-tsan -L stress --output-on-failure
```

Expected: all `stress`-labeled tests PASS with **no** ThreadSanitizer warnings in
the output. If TSan reports a data race in Halcyon production code, stop and fix it
via the systematic-debugging skill (it is a real finding — that is the point of
this suite). If the race is inside the fake/runner test code, fix it there.

- [x] **Step 4: Full build + run under AddressSanitizer**

Run:

```bash
cmake -S . -B build-asan -DHALCYON_BUILD_TESTS=ON \
      -DHALCYON_BUILD_STRESS_TESTS=ON -DHALCYON_SANITIZER=address
cmake --build build-asan -j --target halcyon_stress_tests
ctest --test-dir build-asan -L stress --output-on-failure
```

Expected: all PASS, no ASan reports (especially `DestroyDatabaseWithInflightAsyncWork`).

- [x] **Step 5: Confirm the default build is unaffected**

Run:

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON   # no stress option
cmake --build build -j
ctest --test-dir build --output-on-failure     # stress tests absent; unit suite unchanged
```

Expected: the normal unit suite runs as before; no `stress` tests are built or run.

- [x] **Step 6: Commit**

```bash
git add tests/stress/README.md AGENTS.md
git commit -m "docs: document the concurrency stress + perf suite"
```

---

## Task 13: Integration verification against live Db2 (per AGENTS.md)

**Files:** none (verification only)

AGENTS.md requires running the full suite including live integration before
claiming completion. The perf harness's live backend is the relevant exercise.

- [x] **Step 1: Bring up Db2 and run the existing integration suite**

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON -DHALCYON_BUILD_INTEGRATION_TESTS=ON \
      -DHALCYON_BUILD_STRESS_TESTS=ON
cmake --build build -j
docker compose -f docker/docker-compose.yml up -d
docker compose -f docker/docker-compose.yml ps   # wait for STATUS = healthy
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
ctest --test-dir build -L integration --output-on-failure
```

Expected: integration tests PASS (not Skipped).

- [x] **Step 2: Run the perf harness against live Db2**

```bash
./build/tests/stress/halcyon_stress --backend=live --scenario=pool,executor,cache,txn \
    --threads=4,8,16 --duration=3000
```

Expected: a report with non-zero throughput and zero `FAILED` rows for each
scenario/thread cell. (The `reconnect` scenario is fake-only and is skipped on
live automatically.)

- [x] **Step 3: Tear down**

```bash
docker compose -f docker/docker-compose.yml down
```

- [x] **Step 4: Final commit (if any verification fixes were needed)**

```bash
git add -A
git commit -m "test: verify stress suite end-to-end (TSan/ASan + live Db2 perf)"
```

---

## Notes for the implementer

- **TDD discipline:** every scenario test is written to pass against correct
  production code. A failure (or a sanitizer report) in Tasks 5–10 is most likely a
  real Halcyon concurrency bug — investigate with the systematic-debugging skill and
  fix the production code under `include/halcyon/`, not the test.
- **Determinism:** correctness tests use `total_iters` (not duration) so op counts
  and fault alignment are reproducible. The perf harness uses `duration`.
- **No production API changes** are required by this plan; if you find you need one,
  stop and revisit the spec.
