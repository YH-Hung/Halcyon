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
    auto blk = d.fetchBlock(s.value(), 100);
    ASSERT_TRUE(blk.ok());
    ASSERT_EQ(blk.value().size(), 1u);  // one row
    EXPECT_EQ(std::get<std::int64_t>(blk.value()[0][0]), 7);
    EXPECT_TRUE(d.fetchBlock(s.value(), 100).value().empty());  // end of cursor
}

TEST(FakeDriverTest, FailExecuteEveryTripsAtRate) {
    ConcurrentFakeDriver d;
    d.failExecuteEvery = 2;  // every 2nd execute fails, retriable
    auto c = d.connect({"dsn"}).value();
    auto s = d.prepare(c, "SELECT 1 FROM SYSIBM.SYSDUMMY1").value();
    EXPECT_TRUE(d.execute(s).ok());  // call 1
    auto r2 = d.execute(s);          // call 2 -> fail
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

#include <atomic>

#include "workload_runner.hpp"

using halcyon::stress::run_workload;
using halcyon::stress::RunConfig;
using halcyon::stress::RunReport;
using halcyon::stress::Stop;
using halcyon::stress::WorkerCtx;
using halcyon::stress::Workload;

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
    cfg.threads = 32;  // threads >> pool max
    cfg.stop.total_iters = 20000;
    RunReport r = run_workload(w, cfg);

    EXPECT_FALSE(r.failed) << r.first_error;
    EXPECT_LE(fake->peakInFlight.load(), 8);  // never more leases than max
    EXPECT_EQ(db.value().pool().active_count(), 0u);
    EXPECT_EQ(db.value().pool().idle_count(), db.value().pool().total_count());
}

using halcyon::stress::make_executor_saturation;

TEST(StressScenario, ExecutorSaturationResolvesEveryFuture) {
    auto fake = std::make_shared<ConcurrentFakeDriver>();
    auto db = Database::open(fake, "dsn", config_for(ScenarioId::Executor, 4));
    ASSERT_TRUE(db.ok()) << db.error().message;

    Workload w = make_executor_saturation(db.value());
    RunConfig cfg;
    cfg.threads = 16;  // far more launchers than executor threads
    cfg.stop.total_iters = 8000;
    RunReport r = run_workload(w, cfg);

    EXPECT_FALSE(r.failed) << r.first_error;
    EXPECT_EQ(r.ops, 8000u);  // every op (a launched+awaited future) done
    EXPECT_EQ(db.value().pool().active_count(), 0u);
}

using halcyon::stress::make_cache_churn;

// Thread-safe sink capturing the per-connection statement-cache counters Halcyon
// emits (halcyon_stmt_cache_total{result=hit|miss|overflow|evict} +
// halcyon_stmt_cache_size). Called synchronously and concurrently from every
// leased connection, so counts are atomic and the size-max is mutex-guarded.
struct CacheCountingSink final : halcyon::obs::MetricsSink {
    std::atomic<long> hit{0}, miss{0}, overflow{0};
    std::mutex mu;
    double max_size_ = 0.0;

    void counter(std::string_view name, double value,
                 const halcyon::obs::Labels& labels) override {
        if (name != "halcyon_stmt_cache_total") return;
        for (const auto& kv : labels) {
            if (kv.first != "result") continue;
            const auto n = static_cast<long>(value);
            if (kv.second == "hit") hit.fetch_add(n, std::memory_order_relaxed);
            else if (kv.second == "miss") miss.fetch_add(n, std::memory_order_relaxed);
            else if (kv.second == "overflow") overflow.fetch_add(n, std::memory_order_relaxed);
        }
    }
    void histogram(std::string_view, double,
                   const halcyon::obs::Labels&) override {}
    void gauge(std::string_view name, double value,
               const halcyon::obs::Labels&) override {
        if (name != "halcyon_stmt_cache_size") return;
        std::lock_guard<std::mutex> lk(mu);
        if (value > max_size_) max_size_ = value;
    }
    double max_size() {
        std::lock_guard<std::mutex> lk(mu);
        return max_size_;
    }
};

TEST(StressScenario, StatementCacheStaysCorrectUnderReuse) {
    auto fake = std::make_shared<ConcurrentFakeDriver>();
    auto sink = std::make_shared<CacheCountingSink>();
    halcyon::PoolConfig pc = config_for(ScenarioId::Cache, 2);
    pc.observability.metrics = sink;
    auto db = Database::open(fake, "dsn", pc);
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
    // §6.3: every statement acquire resolves to exactly one of hit/miss/overflow,
    // one per execute — so the three partition the executes exactly.
    const long classified =
        sink->hit.load() + sink->miss.load() + sink->overflow.load();
    EXPECT_EQ(classified, fake->executeCalls.load());
    EXPECT_GT(sink->hit.load(), 0);  // cache hits genuinely occurred
    // Live cache never exceeds the configured per-connection capacity.
    EXPECT_LE(sink->max_size(), static_cast<double>(pc.statementCacheSize));
    EXPECT_EQ(db.value().pool().active_count(), 0u);
}

using halcyon::stress::make_reconnect_faults;

TEST(StressScenario, ReconnectAndRetryRecoverUnderFaults) {
    auto fake = std::make_shared<ConcurrentFakeDriver>();
    fake->failExecuteEvery = 7;      // retriable statement errors
    fake->killConnectionEvery = 11;  // forces validate-on-acquire reconnects
    auto db = Database::open(fake, "dsn", config_for(ScenarioId::Reconnect, 6));
    ASSERT_TRUE(db.ok()) << db.error().message;
    const long connects_after_warmup = fake->connectCalls.load();

    // The safe-retry policy is bounded (3 attempts here). Under ~22% per-attempt
    // fault injection across 12 threads a small minority of ops legitimately
    // exhaust their budget and surface a *retriable* error — correct bounded-policy
    // behaviour (spec §6.4), not a bug. `recovered` proves the bulk genuinely came
    // back with the right value; the workload still fails loudly on a non-retriable
    // error or a wrong scalar (a cross-thread handle/row mixup).
    std::atomic<long> recovered{0};
    Workload w = make_reconnect_faults(db.value(), &recovered);
    RunConfig cfg;
    cfg.threads = 12;
    cfg.stop.total_iters = 30000;
    RunReport r = run_workload(w, cfg);

    EXPECT_FALSE(r.failed) << r.first_error;  // no wrong scalar / non-retriable
    EXPECT_GT(fake->connectCalls.load(),
              connects_after_warmup);  // reconnects actually fired
    EXPECT_GT(fake->executeCalls.load(),
              static_cast<long>(r.ops));  // retries actually fired
    EXPECT_GT(recovered.load(),
              static_cast<long>(r.ops) * 9 / 10);     // vast majority recovered
    EXPECT_EQ(db.value().pool().active_count(), 0u);  // no leaked leases
}

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
    const long txns = fake->autoCommitOff.load();  // each begin() flips off
    EXPECT_EQ(txns, 16000);
    EXPECT_EQ(fake->commitCalls.load() + fake->rollbackCalls.load(), txns);
    EXPECT_EQ(fake->autoCommitOn.load(), txns);       // each tx restores it
    EXPECT_EQ(db.value().pool().active_count(), 0u);  // no tx leaks a connection
}

#include <future>
#include <vector>

// Scenario 6 reuses make_pool_contention (already in scope) under reaper pressure.

// Captures every halcyon_pool_connections{state=active|idle} gauge sample the pool
// emits during the run so the reaper invariants can be checked over the whole run,
// not just at a final quiescent snapshot. Each sample is a consistent point-in-time
// (the pool computes active = slots - idle under its lock). Thread-safe: the
// maintenance thread keeps emitting after the workload joins.
struct PoolGaugeSink final : halcyon::obs::MetricsSink {
    std::mutex mu;
    long samples = 0;
    double min_active = 1e18, max_active = -1.0, min_idle = 1e18;

    void counter(std::string_view, double, const halcyon::obs::Labels&) override {}
    void histogram(std::string_view, double,
                   const halcyon::obs::Labels&) override {}
    void gauge(std::string_view name, double value,
               const halcyon::obs::Labels& labels) override {
        if (name != "halcyon_pool_connections") return;
        std::string_view state;
        for (const auto& kv : labels)
            if (kv.first == "state") state = kv.second;
        std::lock_guard<std::mutex> lk(mu);
        ++samples;
        if (state == "active") {
            if (value < min_active) min_active = value;
            if (value > max_active) max_active = value;
        } else if (state == "idle") {
            if (value < min_idle) min_idle = value;
        }
    }
};

TEST(StressScenario, ReaperRacesAcquireReleaseSafely) {
    auto fake = std::make_shared<ConcurrentFakeDriver>();
    auto gauge = std::make_shared<PoolGaugeSink>();
    // Lifecycle config: maintenance thread on, tiny intervals so the reaper fires
    // constantly while acquires/releases race it.
    halcyon::PoolConfig pc = config_for(ScenarioId::Lifecycle, 6);
    pc.observability.metrics = gauge;
    auto db = Database::open(fake, "dsn", pc);
    ASSERT_TRUE(db.ok()) << db.error().message;

    Workload w = make_pool_contention(db.value());  // acquire/query/release loop
    RunConfig cfg;
    cfg.threads = 16;
    cfg.stop.duration = std::chrono::milliseconds(300);  // let the reaper churn
    RunReport r = run_workload(w, cfg);

    EXPECT_FALSE(r.failed) << r.first_error;
    EXPECT_LE(fake->peakInFlight.load(), 6);  // reaper never reaps a lease
    EXPECT_EQ(db.value().pool().active_count(), 0u);
    EXPECT_GE(db.value().pool().total_count(),
              db.value().pool().idle_count());  // accounting consistent

    // Reaper/gauge invariants over the WHOLE run (§6.6). The maintenance thread is
    // still emitting, so read the captured extrema under the sink's lock.
    std::lock_guard<std::mutex> lk(gauge->mu);
    EXPECT_GT(gauge->samples, 0);       // the gauge was actually exercised
    EXPECT_GE(gauge->min_active, 0.0);  // active count is never negative
    EXPECT_LE(gauge->max_active, 6.0);  // reaper/acquire never exceed pool max
    EXPECT_GE(gauge->min_idle, 0.0);    // idle count is never negative
    // NOTE: total >= min is deliberately NOT asserted at a single snapshot. With
    // maxLifetime=5ms the reaper legitimately evicts a connection even at/below min
    // and refills afterwards (see ConnectionPool::maintain), so total transiently
    // dips below min by design; a point-in-time total >= min check would be flaky.
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
