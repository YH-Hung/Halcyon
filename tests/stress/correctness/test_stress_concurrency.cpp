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

using halcyon::stress::make_reconnect_faults;

TEST(StressScenario, ReconnectAndRetryRecoverUnderFaults) {
    auto fake = std::make_shared<ConcurrentFakeDriver>();
    fake->failExecuteEvery = 7;       // retriable statement errors
    fake->killConnectionEvery = 11;   // forces validate-on-acquire reconnects
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

    EXPECT_FALSE(r.failed) << r.first_error;            // no wrong scalar / non-retriable
    EXPECT_GT(fake->connectCalls.load(),
              connects_after_warmup);                   // reconnects actually fired
    EXPECT_GT(fake->executeCalls.load(),
              static_cast<long>(r.ops));                // retries actually fired
    EXPECT_GT(recovered.load(),
              static_cast<long>(r.ops) * 9 / 10);       // vast majority recovered
    EXPECT_EQ(db.value().pool().active_count(), 0u);    // no leaked leases
}
