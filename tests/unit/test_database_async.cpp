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
