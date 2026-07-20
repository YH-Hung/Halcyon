#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "halcyon/async.hpp"
#include "halcyon/pool.hpp"
#include "halcyon/result.hpp"
#include "mock_cli_driver.hpp"

using halcyon::ConnectionPool;
using halcyon::Executor;
using halcyon::PoolConfig;
using halcyon::Result;
using halcyon::testing::MockCliDriver;

TEST(Executor, SubmitReturnsFutureWithValue) {
    Executor ex(2);
    auto f = ex.submit([] { return 6 * 7; });
    EXPECT_EQ(f.get(), 42);
}

TEST(Executor, RunsManyTasks) {
    Executor ex(4);
    std::vector<std::future<int>> fs;
    for (int i = 0; i < 16; ++i) fs.push_back(ex.submit([i] { return i; }));
    int sum = 0;
    for (auto& f : fs) sum += f.get();
    EXPECT_EQ(sum, 120);
}

TEST(Executor, DrainsOutstandingTasksOnDestruction) {
    std::atomic<int> done{0};
    {
        Executor ex(2);
        for (int i = 0; i < 8; ++i)
            ex.submit([&done] { done.fetch_add(1); });
    }  // dtor joins after draining the queue
    EXPECT_EQ(done.load(), 8);
}

TEST(AsyncWithConnection, RunsQueryOnPooledConnection) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"}, {{halcyon::detail::cli::Value{std::int64_t{5}}}, {halcyon::detail::cli::Value{std::int64_t{9}}}}});
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    Executor ex(2);

    auto f = halcyon::async_with_connection(
        ex, *pool, [](halcyon::Connection& c) -> Result<std::int64_t> {
            auto rs = c.query("SELECT id FROM t");
            if (!rs.ok()) return rs.error();
            std::int64_t sum = 0;
            for (auto& row : rs.value()) sum += std::get<0>(row.as<std::int64_t>());
            return sum;
        });
    auto r = f.get();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 14);
}

// v1.2: destroying the executor ON one of its own workers (a user continuation
// resumed on a worker can destroy the last Database copy) must not self-join.
// The queue must still drain: joined workers exit only at stop-and-empty, and
// the detached self keeps looping over the co-owned State after ~Executor.
TEST(Executor, DestructionOnOwnWorkerDetachesAndDrainsQueue) {
    auto* ex = new halcyon::Executor(1);
    std::promise<void> gateEntered, gateRelease, drained;
    auto gateEnteredF = gateEntered.get_future();
    auto gateReleaseF = gateRelease.get_future();
    auto drainedF = drained.get_future();

    // Park the single worker so all three jobs are enqueued in order.
    ex->submit([&] {
        gateEntered.set_value();
        gateReleaseF.wait();
    });
    ex->submit([&] { delete ex; });            // ~Executor runs ON the worker
    ex->submit([&] { drained.set_value(); });  // enqueued-before-stop: must run

    gateEnteredF.wait();
    gateRelease.set_value();
    ASSERT_EQ(drainedF.wait_for(std::chrono::seconds(5)),
              std::future_status::ready)
        << "queue not drained after teardown-on-worker";
    // Give the detached worker a beat to exit its loop before gtest tears down.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// Regression: off-worker destruction still joins and drains everything.
TEST(Executor, DestructionOffWorkerJoinsAndDrains) {
    std::atomic<int> ran{0};
    {
        halcyon::Executor ex(2);
        for (int i = 0; i < 32; ++i)
            ex.submit([&] { ++ran; });
    }  // dtor joins both workers
    EXPECT_EQ(ran.load(), 32);
}

TEST(Executor, PostRunsInFifoOrderWithSubmit) {
    halcyon::Executor ex(1);
    std::promise<void> gateEntered, gateRelease;
    auto gateEnteredF = gateEntered.get_future();
    auto gateReleaseF = gateRelease.get_future();
    ex.submit([&] { gateEntered.set_value(); gateReleaseF.wait(); });
    gateEnteredF.wait();  // worker parked: everything below queues in order

    std::vector<int> order;
    std::mutex om;
    EXPECT_TRUE(ex.post([&]() noexcept {
        std::lock_guard<std::mutex> lk(om);
        order.push_back(1);
    }));
    auto f = ex.submit([&] {
        std::lock_guard<std::mutex> lk(om);
        order.push_back(2);
    });
    EXPECT_TRUE(ex.post([&]() noexcept {
        std::lock_guard<std::mutex> lk(om);
        order.push_back(3);
    }));
    gateRelease.set_value();
    f.wait();  // 1 ran before 2; drain 3 by queueing a 4th and waiting on it
    ex.submit([] {}).wait();
    std::lock_guard<std::mutex> lk(om);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(Executor, OnWorkerThreadTrueOnlyOnWorkers) {
    EXPECT_FALSE(halcyon::Executor::onWorkerThread());
    halcyon::Executor ex(1);
    auto f = ex.submit([] { return halcyon::Executor::onWorkerThread(); });
    EXPECT_TRUE(f.get());
    EXPECT_FALSE(halcyon::Executor::onWorkerThread());
}

// A live State does NOT imply posting is permitted: workers co-own the block
// past stop. post must check stop under the State mutex and reject.
TEST(Executor, PostAfterStopIsRejectedNotStranded) {
    std::shared_ptr<halcyon::Executor::State> state;
    {
        halcyon::Executor ex(1);
        state = ex.weakState().lock();
        ASSERT_TRUE(state);
    }  // dtor: stop set, worker joined; `state` (our strong ref) keeps the block alive
    bool ran = false;
    EXPECT_FALSE(state->post([&ran]() noexcept { ran = true; }));
    EXPECT_FALSE(ran);
}

TEST(Executor, WeakStateExpiresAfterHandleAndWorkersExit) {
    std::weak_ptr<halcyon::Executor::State> w;
    {
        halcyon::Executor ex(2);
        w = ex.weakState();
        EXPECT_FALSE(w.expired());
    }
    EXPECT_TRUE(w.expired());
}

// The lock-vs-destroy race: a strong State ref taken ON a worker while another
// thread destroys the last handle must neither deadlock nor leak (run the
// suite under ASan to prove the latter).
TEST(Executor, StateLockedOnWorkerWhileHandleDestroyedElsewhere) {
    auto ex = std::make_unique<halcyon::Executor>(1);
    std::promise<void> locked, release;
    auto lockedF = locked.get_future();
    auto releaseF = release.get_future();
    std::shared_ptr<halcyon::Executor::State> stateOnWorker;
    auto w = ex->weakState();
    ASSERT_TRUE(ex->post([&, w]() noexcept {
        stateOnWorker = w.lock();  // strong State ref taken ON the worker
        locked.set_value();
        releaseF.wait();
    }));
    lockedF.wait();
    std::thread killer([&] { ex.reset(); });  // dtor blocks joining the parked worker
    release.set_value();                      // worker finishes; join completes
    killer.join();
    EXPECT_TRUE(stateOnWorker != nullptr);
    stateOnWorker.reset();  // releasing the last-but-one ref controls no threads
}
