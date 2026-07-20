#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "halcyon/coro.hpp"
#include "halcyon/observability/tracing.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::ErrorCode;
using halcyon::PoolConfig;
using halcyon::Result;
using halcyon::Transaction;
using halcyon::coro::syncWait;
using halcyon::coro::Task;
using halcyon::testing::MockCliDriver;

namespace {

PoolConfig noThread() {
    PoolConfig c;
    c.startMaintenanceThread = false;
    return c;
}

// A foreign awaitable: resumes the coroutine on a brand-new non-Halcyon
// thread (joined by the test for sanitizer hygiene).
struct ForeignResume {
    std::thread* out;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const {
        *out = std::thread([h] { h.resume(); });
    }
    void await_resume() const noexcept {}
};

// Adapter whose attachContext throws from the Nth call on (1-based), so a
// test can let earlier attaches succeed (e.g. the begin offload's) and fail
// a specific later one (e.g. the hop-back's).
struct CountingFlakyTracer final : halcyon::obs::Tracer {
    std::atomic<int> attachCalls{0};
    int throwFromCall = 1000;
    std::unique_ptr<halcyon::obs::Span> startSpan(
        std::string_view, const halcyon::obs::SpanAttrs&) override {
        return std::make_unique<halcyon::obs::NoopSpan>();
    }
    std::unique_ptr<halcyon::obs::ContextToken> attachContext(
        const std::shared_ptr<halcyon::obs::SpanContext>&) override {
        if (attachCalls.fetch_add(1) + 1 >= throwFromCall)
            throw std::runtime_error("attach failed");
        return nullptr;
    }
};

}  // namespace

TEST(CoroTransaction, PlainFnCommitsOnOk) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    auto db = Database::open(driver, "X", noThread()).value();
    auto r = syncWait(halcyon::coro::transaction(db, [](Transaction& tx) {
        auto e = tx.execute("INSERT INTO t VALUES (?)", 1);
        if (!e.ok()) return Result<int>(e.error());
        return Result<int>(42);
    }));
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value(), 42);
    EXPECT_EQ(driver.commitCalls, 1);
    EXPECT_EQ(driver.rollbackCalls, 0);
}

TEST(CoroTransaction, PlainFnRollsBackOnErrorResult) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    auto r = syncWait(halcyon::coro::transaction(db, [](Transaction&) {
        halcyon::Error e;
        e.code = ErrorCode::Constraint;
        e.message = "dup";
        return Result<int>(e);
    }));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Constraint);
    EXPECT_EQ(driver.commitCalls, 0);
    EXPECT_EQ(driver.rollbackCalls, 1);
}

TEST(CoroTransaction, PlainFnExceptionRollsBackThenRethrows) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    EXPECT_THROW(
        syncWait(halcyon::coro::transaction(
            db,
            [](Transaction&) -> Result<int> { throw std::runtime_error("boom"); })),
        std::runtime_error);
    EXPECT_EQ(driver.commitCalls, 0);
    EXPECT_GE(driver.rollbackCalls, 1);
}

TEST(CoroTransaction, PlainFnIsolationOverloadSetsIsolation) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    auto r = syncWait(halcyon::coro::transaction(
        db, halcyon::Isolation::ReadStability,
        [](Transaction&) { return Result<int>(1); }));
    ASSERT_TRUE(r.ok());
    ASSERT_GE(driver.isolationCalls.size(), 1u);
    EXPECT_EQ(driver.isolationCalls[0].second, halcyon::Isolation::ReadStability);
}

// A commit failure must surface as the transaction's error and poison-discard
// the pooled connection (ScopedTransaction semantics, inherited). txnErrors
// fails the NEXT txn call — queueing it up front would fail begin's
// setAutoCommit(false) — so the commitHook injects it just-in-time.
TEST(CoroTransaction, PlainFnCommitFailurePoisonsAndSurfaces) {
    MockCliDriver driver;
    halcyon::Error dead;
    dead.code = ErrorCode::Connection;
    dead.message = "lost";
    driver.commitHook = [&] { driver.txnErrors.push_back(dead); };
    auto db = Database::open(driver, "X", noThread()).value();
    auto r = syncWait(halcyon::coro::transaction(
        db, [](Transaction&) { return Result<int>(1); }));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Connection);
    EXPECT_EQ(db.pool().total_count(), 0u);  // poisoned lease was discarded
}

// Coroutine-fn: after a foreign awaitable, fn runs on a foreign thread; the
// scope must hop commit back onto a Halcyon worker.
TEST(CoroTransaction, CoroutineFnCommitHopsBackToWorker) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    auto db = Database::open(driver, "X", noThread()).value();
    bool commitOnWorker = false;
    driver.commitHook = [&] {
        commitOnWorker = halcyon::Executor::onWorkerThread();
    };
    std::thread foreign;
    bool fnSawForeignThread = false;

    auto fn = [&](Transaction& tx) -> Task<Result<int>> {
        co_await ForeignResume{&foreign};
        fnSawForeignThread = !halcyon::Executor::onWorkerThread();
        auto e = tx.execute("INSERT INTO t VALUES (?)", 1);  // blocks the foreign thread (documented)
        if (!e.ok()) co_return Result<int>(e.error());
        co_return Result<int>(7);
    };
    auto r = syncWait(halcyon::coro::transaction(db, fn));
    foreign.join();
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value(), 7);
    EXPECT_TRUE(fnSawForeignThread);
    EXPECT_TRUE(commitOnWorker);  // terminal ops confined to Halcyon workers
    EXPECT_EQ(driver.commitCalls, 1);
}

TEST(CoroTransaction, CoroutineFnErrorRollsBack) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    std::thread foreign;
    auto fn = [&](Transaction&) -> Task<Result<int>> {
        co_await ForeignResume{&foreign};
        halcyon::Error e;
        e.code = ErrorCode::Constraint;
        e.message = "no";
        co_return Result<int>(e);
    };
    auto r = syncWait(halcyon::coro::transaction(db, fn));
    foreign.join();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Constraint);
    EXPECT_EQ(driver.commitCalls, 0);
    EXPECT_EQ(driver.rollbackCalls, 1);
}

TEST(CoroTransaction, CoroutineFnExceptionRollsBackThenRethrows) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    std::thread foreign;
    auto fn = [&](Transaction&) -> Task<Result<int>> {
        co_await ForeignResume{&foreign};
        throw std::runtime_error("mid-txn boom");
        co_return Result<int>(0);
    };
    EXPECT_THROW(syncWait(halcyon::coro::transaction(db, fn)),
                 std::runtime_error);
    foreign.join();
    EXPECT_EQ(driver.commitCalls, 0);
    EXPECT_EQ(driver.rollbackCalls, 1);
}

// Executor-gone edge: the hop-back submit fails, so the scope rolls back
// synchronously on the current (foreign) thread rather than leak an open
// transaction, and reports InvalidState. Bounded wait guards a hang.
TEST(CoroTransaction, ExecutorGoneBeforeCommitRollsBackSynchronously) {
    MockCliDriver driver;
    auto db = std::make_optional(Database::open(driver, "X", noThread()).value());
    std::thread foreign;
    auto fn = [&](Transaction&) -> Task<Result<int>> {
        co_await ForeignResume{&foreign};
        db.reset();  // last Database copy dies mid-transaction (foreign thread)
        co_return Result<int>(42);
    };
    auto t = halcyon::coro::transaction(*db, fn);
    auto fut = std::async(std::launch::async,
                          [&] { return syncWait(std::move(t)); });
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    foreign.join();
    auto r = fut.get();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidState);
    EXPECT_EQ(driver.commitCalls, 0);
    EXPECT_EQ(driver.rollbackCalls, 1);  // rolled back, not leaked
}

// HopToWorker's context attachment is BEST-EFFORT — the contractual
// carve-out from the rethrow-at-co_await rule (spec §7): a tracing failure
// at the hop degrades tracing, never the terminal commit. Attach call #1 is
// the begin offload's (must succeed so the transaction opens); attach #2 is
// the hop-back's (throws).
TEST(CoroTransaction, HopAttachFailureDoesNotAbortCommit) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    auto tracer = std::make_shared<CountingFlakyTracer>();
    tracer->throwFromCall = 2;  // begin-offload attach ok; hop attach throws
    PoolConfig cfg = noThread();
    cfg.observability.tracer = tracer;
    auto db = Database::open(driver, "X", cfg).value();
    std::thread foreign;
    auto fn = [&](Transaction& tx) -> Task<Result<int>> {
        co_await ForeignResume{&foreign};
        auto e = tx.execute("INSERT INTO t VALUES (?)", 1);
        if (!e.ok()) co_return Result<int>(e.error());
        co_return Result<int>(7);
    };
    auto r = syncWait(halcyon::coro::transaction(db, fn));
    foreign.join();
    ASSERT_TRUE(r.ok()) << r.error().message;  // commit went through anyway
    EXPECT_EQ(r.value(), 7);
    EXPECT_EQ(driver.commitCalls, 1);
    EXPECT_EQ(driver.rollbackCalls, 0);
    EXPECT_GE(tracer->attachCalls.load(), 2);  // the hop DID attempt to attach
}
