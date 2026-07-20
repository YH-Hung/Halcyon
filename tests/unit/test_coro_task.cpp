#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <utility>

#include "halcyon/coro.hpp"

using halcyon::coro::syncWait;
using halcyon::coro::Task;

namespace {

Task<int> flag_task(bool* ran) {
    if (ran != nullptr) *ran = true;
    co_return 7;
}

Task<void> void_flag_task(bool* ran) {
    *ran = true;
    co_return;
}

Task<void> throwing_task() {
    throw std::runtime_error("boom");
    co_return;  // makes this function a coroutine; unreachable
}

Task<int> add_chain(int depth) {
    if (depth == 0) co_return 0;
    co_return 1 + co_await add_chain(depth - 1);
}

}  // namespace

TEST(CoroTask, LazyUntilAwaited) {
    bool ran = false;
    auto t = flag_task(&ran);
    EXPECT_FALSE(ran);  // nothing ran at creation
    EXPECT_EQ(syncWait(std::move(t)), 7);
    EXPECT_TRUE(ran);
}

TEST(CoroTask, NeverAwaitedNeverRunsAndDestroysCleanly) {
    bool ran = false;
    {
        auto t = flag_task(&ran);
        (void)t;
    }  // destroyed while suspended at initial_suspend
    EXPECT_FALSE(ran);
}

TEST(CoroTask, MoveTransfersOwnership) {
    bool ran = false;
    auto t = flag_task(&ran);
    Task<int> u = std::move(t);
    EXPECT_EQ(syncWait(std::move(u)), 7);
}

TEST(CoroTask, ExceptionPropagatesOutOfCoAwait) {
    EXPECT_THROW(syncWait(throwing_task()), std::runtime_error);
}

TEST(CoroTask, TaskOfVoidCompletes) {
    bool ran = false;
    syncWait(void_flag_task(&ran));
    EXPECT_TRUE(ran);
}

// Symmetric transfer is required, not optional: naive resumption overflows the
// native stack on long chains. ~100k-deep await chain must complete.
TEST(CoroTask, SymmetricTransferSurvivesDeepChains) {
    EXPECT_EQ(syncWait(add_chain(100000)), 100000);
}

#if !defined(NDEBUG) && defined(GTEST_HAS_DEATH_TEST)
// syncWait on an executor worker risks self-deadlock (documented); debug
// builds assert via Executor::onWorkerThread().
TEST(CoroSyncWaitDeathTest, AssertsOnExecutorWorkerThread) {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_DEATH(
        {
            halcyon::Executor ex(1);
            ex.submit([] { syncWait(flag_task(nullptr)); }).get();
        },
        "executor worker");
}
#endif
