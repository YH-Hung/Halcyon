#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "halcyon/database.hpp"
#include "halcyon/observability/tracing.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;

namespace {
PoolConfig noThread() {
    PoolConfig c;
    c.startMaintenanceThread = false;
    return c;
}
}  // namespace

struct Num {
    std::int64_t n;
};
HALCYON_REFLECT(Num, n);

TEST(DatabaseAsync, ExecuteAsyncReturnsCount) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(4);
    auto db = Database::open(driver, "X", noThread()).value();
    auto f = db.executeAsync("UPDATE t SET a=? WHERE id=?", 1, 2);
    auto r = f.get();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 4);
}

// The Database is a copyable handle: copies share the pool/executor. An async
// task must not depend on the *specific* handle that launched it — destroying
// that handle while another copy keeps the executor alive must be safe (no
// use-after-free). Run under ASan to detect the dangling-`this` capture.
TEST(DatabaseAsync, TaskOutlivesLaunchingDatabaseCopy) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(0);  // barrier task's execute
    driver.execRowCounts.push_back(7);  // the task under test

    std::promise<void> barrierEntered;
    std::promise<void> releaseBarrier;
    auto enteredFut = barrierEntered.get_future();
    auto releaseFut = releaseBarrier.get_future();
    int calls = 0;
    driver.executeHook = [&]() {
        if (calls++ == 0) {  // only the first (barrier) execute blocks
            barrierEntered.set_value();
            releaseFut.wait();
        }
    };

    PoolConfig cfg = noThread();
    cfg.max = 1;                                         // single executor thread → deterministic task queueing
    auto db = Database::open(driver, "X", cfg).value();  // canonical: keeps backing alive

    // Occupy the one worker so the task under test queues behind it.
    auto barrier = db.executeAsync("BARRIER", 0);
    enteredFut.wait();  // worker is now parked inside the barrier's execute

    auto launcher = std::make_unique<Database>(db);          // a separate handle copy
    auto f = launcher->executeAsync("UPDATE t SET a=?", 1);  // queued behind barrier
    launcher.reset();                                        // destroy the launching handle *before* its task runs

    releaseBarrier.set_value();  // worker finishes barrier, then runs the test task
    ASSERT_TRUE(barrier.get().ok());

    auto r = f.get();
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value(), 7);
}

TEST(DatabaseAsync, QueryAsyncMaterializesRows) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"n"},
        {{halcyon::detail::cli::Value{std::int64_t{2}}},
         {halcyon::detail::cli::Value{std::int64_t{5}}}}});
    auto db = Database::open(driver, "X", noThread()).value();
    auto f = db.queryAsync<Num>("SELECT n FROM t");
    auto r = f.get();
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().size(), 2u);
    EXPECT_EQ(r.value()[1].n, 5);
}

// v1.2: with detach-self teardown, queued future work can outlive ~Database.
// Each job must carry its own driver+pool backing: destroy the LAST Database
// copy ON the worker (inside a running job) while more executeAsync jobs are
// queued — every job must still complete against a live driver (ASan-clean).
TEST(DatabaseAsync, QueuedFutureWorkSurvivesDatabaseDestructionOnWorker) {
    auto driver = std::make_shared<MockCliDriver>();
    auto* drv = driver.get();         // the test must NOT co-own the driver: the jobs'
                                      // backing is what has to keep it alive
    drv->execRowCounts.push_back(0);  // gate job
    drv->execRowCounts.push_back(3);  // job that destroys the Database
    drv->execRowCounts.push_back(9);  // job drained after teardown

    std::promise<void> gateEntered, gateRelease;
    auto gateEnteredF = gateEntered.get_future();
    auto gateReleaseF = gateRelease.get_future();
    std::optional<Database> holder;
    int calls = 0;
    drv->executeHook = [&]() {
        const int n = calls++;
        if (n == 0) {
            gateEntered.set_value();
            gateReleaseF.wait();
        }
        if (n == 1) holder.reset();  // last Database copy dies ON the worker
    };

    PoolConfig cfg = noThread();
    cfg.max = 1;  // one worker; deterministic job order
    holder.emplace(Database::open(std::move(driver), "X", cfg).value());

    auto gate = holder->executeAsync("GATE", 0);
    gateEnteredF.wait();  // worker parked inside the gate job
    auto f1 = holder->executeAsync("UPDATE t SET a=?", 1);
    auto f2 = holder->executeAsync("UPDATE t SET b=?", 2);
    gateRelease.set_value();

    ASSERT_TRUE(gate.get().ok());
    auto r1 = f1.get();
    ASSERT_TRUE(r1.ok()) << r1.error().message;
    EXPECT_EQ(r1.value(), 3);
    auto r2 = f2.get();
    ASSERT_TRUE(r2.ok()) << r2.error().message;
    EXPECT_EQ(r2.value(), 9);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // detached worker exits
}

namespace {
// Minimal capturing tracer: records captureContext()/attachContext() calls so
// tests can assert capture-at-call-time parity with executeAsync.
struct MarkerContext final : halcyon::obs::SpanContext {};
struct CapturingTracer final : halcyon::obs::Tracer {
    std::shared_ptr<halcyon::obs::SpanContext> toCapture =
        std::make_shared<MarkerContext>();
    int captureCalls = 0;
    std::vector<const halcyon::obs::SpanContext*> attached;
    std::unique_ptr<halcyon::obs::Span> startSpan(
        std::string_view, const halcyon::obs::SpanAttrs&) override {
        return std::make_unique<halcyon::obs::NoopSpan>();
    }
    std::shared_ptr<halcyon::obs::SpanContext> captureContext() override {
        ++captureCalls;
        return toCapture;
    }
    std::unique_ptr<halcyon::obs::ContextToken> attachContext(
        const std::shared_ptr<halcyon::obs::SpanContext>& ctx) override {
        attached.push_back(ctx.get());
        return nullptr;
    }
};
}  // namespace

TEST(AsyncBacking, SyncViewServesSyncAndRejectsAsync) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(5);
    auto db = Database::open(driver, "X", noThread()).value();
    auto b = db.asyncBacking();

    auto r = b.syncView.execute("UPDATE t SET a=?", 1);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 5);

    auto ar = b.syncView.executeAsync("UPDATE t SET a=?", 1).get();
    ASSERT_FALSE(ar.ok());
    EXPECT_EQ(ar.error().code, halcyon::ErrorCode::InvalidState);

    auto qr = b.syncView.queryAsync<Num>("SELECT n FROM t").get();
    ASSERT_FALSE(qr.ok());
    EXPECT_EQ(qr.error().code, halcyon::ErrorCode::InvalidState);
}

// The bundle keeps pool (and driver) alive but must NOT keep the executor
// alive: exec is weak and expires with the last real Database copy.
TEST(AsyncBacking, BundleOutlivesHandlesButNotExecutor) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(2);
    std::optional<Database::AsyncBacking> b;
    {
        auto db = Database::open(driver, "X", noThread()).value();
        b.emplace(db.asyncBacking());
        EXPECT_FALSE(b->exec.expired());
    }  // last real copy gone: executor stopped and destroyed
    EXPECT_TRUE(b->exec.expired());
    auto r = b->syncView.execute("UPDATE t SET a=?", 1);  // pool still live
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 2);
}

// Destruction order: a shared-driver Database destroyed while a bundle is
// held; ops still complete; bundle teardown destroys pool BEFORE the driver
// (ConnectionPool holds a raw ICliDriver*). The test must NOT co-own the
// driver — an external strong reference would keep it alive and make even a
// wrong member order pass — so lifetime is observed through a weak_ptr and a
// hook counter, and the run is repeated under ASan (Task 3's build-asan-unit)
// where a wrong order is a hard heap-use-after-free.
TEST(AsyncBacking, BundleTeardownDestroysPoolBeforeDriver) {
    auto driver = std::make_shared<MockCliDriver>();
    std::weak_ptr<MockCliDriver> alive = driver;
    int disconnects = 0;
    driver->disconnectHook = [&] { ++disconnects; };
    driver->execRowCounts.push_back(1);
    std::optional<Database::AsyncBacking> b;
    {
        auto db = Database::open(std::move(driver), "X", noThread()).value();
        b.emplace(db.asyncBacking());
    }
    EXPECT_FALSE(alive.expired());  // the bundle alone keeps the driver alive
    ASSERT_TRUE(b->syncView.execute("UPDATE t SET a=?", 1).ok());
    b.reset();                     // teardown: pool disconnects through a live driver, THEN it dies
    EXPECT_GE(disconnects, 1);     // the pool got to tear its connections down
    EXPECT_TRUE(alive.expired());  // ...and only afterwards the driver died
}

TEST(AsyncBacking, CapturesTraceContextAtCallTime) {
    MockCliDriver driver;
    auto tracer = std::make_shared<CapturingTracer>();
    PoolConfig cfg = noThread();
    cfg.observability.tracer = tracer;
    auto db = Database::open(driver, "X", cfg).value();

    EXPECT_EQ(tracer->captureCalls, 0);
    auto b = db.asyncBacking();
    EXPECT_EQ(tracer->captureCalls, 1);  // captured HERE, not at await time
    EXPECT_EQ(b.traceContext.get(), tracer->toCapture.get());

    // Worker-side attachment uses the same public seam the futures use:
    auto guard = b.syncView.useParentContext(b.traceContext);
    ASSERT_EQ(tracer->attached.size(), 1u);
    EXPECT_EQ(tracer->attached[0], tracer->toCapture.get());
}
