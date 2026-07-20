#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "halcyon/coro.hpp"
#include "halcyon/lob.hpp"
#include "halcyon/observability/tracing.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::ErrorCode;
using halcyon::PoolConfig;
using halcyon::coro::syncWait;
using halcyon::coro::Task;
using halcyon::testing::MockCliDriver;

namespace {

PoolConfig noThread() {
    PoolConfig c;
    c.startMaintenanceThread = false;
    return c;
}

struct MarkerContext final : halcyon::obs::SpanContext {};
struct CapturingTracer final : halcyon::obs::Tracer {
    std::shared_ptr<halcyon::obs::SpanContext> toCapture =
        std::make_shared<MarkerContext>();
    std::vector<const halcyon::obs::SpanContext*> attached;
    std::unique_ptr<halcyon::obs::Span> startSpan(
        std::string_view, const halcyon::obs::SpanAttrs&) override {
        return std::make_unique<halcyon::obs::NoopSpan>();
    }
    std::shared_ptr<halcyon::obs::SpanContext> captureContext() override {
        return toCapture;
    }
    std::unique_ptr<halcyon::obs::ContextToken> attachContext(
        const std::shared_ptr<halcyon::obs::SpanContext>& ctx) override {
        attached.push_back(ctx.get());
        return nullptr;
    }
};

// Thread-local "active context" model for the chain test: attachContext makes
// a context active on the calling thread; captureContext snapshots it. This
// is the minimal behavior of the real OTel adapter needed to prove chained
// factories on worker threads keep capturing the ORIGINAL parent.
thread_local const halcyon::obs::SpanContext* g_activeCtx = nullptr;

struct RestoreToken final : halcyon::obs::ContextToken {
    const halcyon::obs::SpanContext* prev;
    explicit RestoreToken(const halcyon::obs::SpanContext* p) : prev(p) {}
    ~RestoreToken() override { g_activeCtx = prev; }
};

struct ChainTracer final : halcyon::obs::Tracer {
    std::shared_ptr<halcyon::obs::SpanContext> root =
        std::make_shared<MarkerContext>();
    std::mutex m;
    std::vector<const halcyon::obs::SpanContext*> captures;

    std::unique_ptr<halcyon::obs::Span> startSpan(
        std::string_view, const halcyon::obs::SpanAttrs&) override {
        return std::make_unique<halcyon::obs::NoopSpan>();
    }
    std::shared_ptr<halcyon::obs::SpanContext> captureContext() override {
        std::lock_guard<std::mutex> lk(m);
        captures.push_back(g_activeCtx);
        return g_activeCtx == root.get()
                   ? root
                   : std::shared_ptr<halcyon::obs::SpanContext>{};
    }
    std::unique_ptr<halcyon::obs::ContextToken> attachContext(
        const std::shared_ptr<halcyon::obs::SpanContext>& ctx) override {
        auto tok = std::make_unique<RestoreToken>(g_activeCtx);
        g_activeCtx = ctx.get();
        return tok;
    }
};

// Adapter whose attachContext throws — attachment runs inside a noexcept
// worker closure, so an uncontained throw would be std::terminate.
struct ThrowingAttachTracer final : halcyon::obs::Tracer {
    std::unique_ptr<halcyon::obs::Span> startSpan(
        std::string_view, const halcyon::obs::SpanAttrs&) override {
        return std::make_unique<halcyon::obs::NoopSpan>();
    }
    std::shared_ptr<halcyon::obs::SpanContext> captureContext() override {
        return std::make_shared<MarkerContext>();
    }
    std::unique_ptr<halcyon::obs::ContextToken> attachContext(
        const std::shared_ptr<halcyon::obs::SpanContext>&) override {
        throw std::runtime_error("attach failed");
    }
};

}  // namespace

TEST(CoroExecute, ParityWithSyncExecute) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(4);
    auto db = Database::open(driver, "X", noThread()).value();
    auto r = syncWait(
        halcyon::coro::execute(db, "UPDATE t SET a=? WHERE id=?", 1, 2));
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value(), 4);
    ASSERT_EQ(driver.statements.size(), 1u);
    const auto& bound = driver.statements.begin()->second.boundParams;
    ASSERT_EQ(bound.size(), 2u);
    EXPECT_EQ(std::get<std::int64_t>(bound[0]), 1);
    EXPECT_EQ(std::get<std::int64_t>(bound[1]), 2);
}

TEST(CoroExecute, NamedParamsOverload) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    auto db = Database::open(driver, "X", noThread()).value();
    auto r = syncWait(halcyon::coro::execute(
        db, "UPDATE t SET a=:v WHERE id=:id",
        halcyon::params{{"v", 10}, {"id", 3}}));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(driver.preparedSql.at(0), "UPDATE t SET a=? WHERE id=?");
}

TEST(CoroExecute, ResumesOnExecutorWorkerThread) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    PoolConfig cfg = noThread();
    cfg.max = 1;  // exactly one worker
    auto db = Database::open(driver, "X", cfg).value();
    std::thread::id workerId{};
    driver.executeHook = [&] { workerId = std::this_thread::get_id(); };

    auto probe = [](Database& d) -> Task<std::thread::id> {
        (void)co_await halcyon::coro::execute(d, "UPDATE t SET a=?", 1);
        co_return std::this_thread::get_id();  // where the continuation ran
    };
    auto resumedOn = syncWait(probe(db));
    EXPECT_EQ(resumedOn, workerId);
    EXPECT_NE(resumedOn, std::this_thread::get_id());
}

TEST(CoroExecute, AcquireFailureShortCircuits) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 0;
    cfg.max = 1;
    cfg.backoff.maxAttempts = 1;
    auto db = Database::open(driver, "X", cfg).value();
    halcyon::Error dead;
    dead.code = ErrorCode::Connection;
    dead.message = "down";
    dead.retriable = false;
    driver.connectErrors.push_back(dead);
    auto r = syncWait(halcyon::coro::execute(db, "UPDATE t SET a=?", 1));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Connection);
}

// The adversarial lifetime test: a string_view whose buffer is freed before
// the first co_await must bind correctly (owned-Value materialization at the
// factory; a decay-copy alone would still be a view).
TEST(CoroExecute, OwnsArgumentsBeforeFirstAwait) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    auto db = Database::open(driver, "X", noThread()).value();
    std::optional<Task<halcyon::Result<std::int64_t>>> t;
    {
        auto big = std::make_unique<std::string>("transient-value");
        std::string_view sv(*big);
        t.emplace(halcyon::coro::execute(db, "INSERT INTO t(name) VALUES (?)", sv));
    }  // buffer freed HERE; the task has not run yet
    auto r = syncWait(std::move(*t));
    ASSERT_TRUE(r.ok());
    const auto& bound = driver.statements.begin()->second.boundParams;
    ASSERT_EQ(bound.size(), 1u);
    EXPECT_EQ(std::get<std::string>(bound[0]), "transient-value");
}

// Weak-executor semantics: awaiting after the last Database copy is gone is a
// defined error, not UB (futures never had this case — they submit eagerly).
TEST(CoroExecute, AwaitAfterLastDatabaseCopyReportsInvalidState) {
    MockCliDriver driver;
    std::optional<Task<halcyon::Result<std::int64_t>>> t;
    {
        auto db = Database::open(driver, "X", noThread()).value();
        t.emplace(halcyon::coro::execute(db, "UPDATE t SET a=?", 1));
    }  // executor stopped and destroyed
    auto r = syncWait(std::move(*t));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidState);
}

// Live State but stopped executor (a worker co-owns the block past stop):
// post must reject and the await must complete — bounded wait so a
// stranded-suspension regression FAILS instead of hanging the suite.
TEST(CoroExecute, PostAfterStopRejectsAwaitBounded) {
    MockCliDriver driver;
    std::shared_ptr<halcyon::Executor::State> keepAlive;
    std::optional<Task<halcyon::Result<std::int64_t>>> t;
    {
        auto db = Database::open(driver, "X", noThread()).value();
        keepAlive = db.asyncBacking().exec.lock();
        ASSERT_TRUE(keepAlive);
        t.emplace(halcyon::coro::execute(db, "UPDATE t SET a=?", 1));
    }
    auto fut = std::async(std::launch::async,
                          [&] { return syncWait(std::move(*t)); });
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)),
              std::future_status::ready)
        << "await stranded: post-after-stop must reject, not enqueue";
    auto r = fut.get();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidState);
}

// In-flight task drains safely when the last Database copy is destroyed
// off-worker mid-operation (~Executor joins → queue drains → result arrives).
TEST(CoroExecute, InFlightTaskDrainsAcrossDatabaseDestruction) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(8);
    std::promise<void> entered, release;
    auto enteredF = entered.get_future();
    auto releaseF = release.get_future();
    driver.executeHook = [&] {
        entered.set_value();
        releaseF.wait();
    };
    auto db = std::make_optional(Database::open(driver, "X", noThread()).value());
    auto t = halcyon::coro::execute(*db, "UPDATE t SET a=?", 1);
    auto waiter = std::async(std::launch::async,
                             [&] { return syncWait(std::move(t)); });
    enteredF.wait();                          // op is running on the worker
    std::thread killer([&] { db.reset(); });  // ~Executor blocks to drain
    release.set_value();
    killer.join();
    auto r = waiter.get();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 8);
}

// Exception containment: a throw inside the offloaded op must rethrow from
// co_await — never escape as std::terminate on the worker (post's contract).
TEST(CoroExecute, ExceptionInsideOffloadedOpRethrownAtCoAwait) {
    MockCliDriver driver;
    driver.executeHook = [] { throw std::runtime_error("driver blew up"); };
    auto db = Database::open(driver, "X", noThread()).value();
    EXPECT_THROW(syncWait(halcyon::coro::execute(db, "UPDATE t SET a=?", 1)),
                 std::runtime_error);
}

// OTel parity: the context captured at the factory call is attached on the
// worker around the offloaded op — same capture point as executeAsync.
TEST(CoroExecute, AttachesCapturedTraceContextOnWorker) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    auto tracer = std::make_shared<CapturingTracer>();
    PoolConfig cfg = noThread();
    cfg.observability.tracer = tracer;
    auto db = Database::open(driver, "X", cfg).value();
    auto r = syncWait(halcyon::coro::execute(db, "UPDATE t SET a=?", 1));
    ASSERT_TRUE(r.ok());
    ASSERT_GE(tracer->attached.size(), 1u);
    EXPECT_EQ(tracer->attached[0], tracer->toCapture.get());
}

// Two-op chain: the SECOND factory runs inside the continuation on a worker
// thread. The worker closure must keep the captured context attached ACROSS
// h.resume(), or the second factory would capture an orphan (null) context
// instead of the original parent.
TEST(CoroExecute, ChainedFactoriesCaptureOriginalParentContext) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    driver.execRowCounts.push_back(1);
    auto tracer = std::make_shared<ChainTracer>();
    PoolConfig cfg = noThread();
    cfg.observability.tracer = tracer;
    auto db = Database::open(driver, "X", cfg).value();

    auto chain = [](Database& d) -> Task<halcyon::Result<std::int64_t>> {
        auto a = co_await halcyon::coro::execute(d, "UPDATE t SET a=?", 1);
        if (!a.ok()) co_return a.error();
        co_return co_await halcyon::coro::execute(d, "UPDATE t SET b=?", 2);
    };

    auto rootGuard = db.useParentContext(tracer->root);  // active on MAIN
    auto r = syncWait(chain(db));
    ASSERT_TRUE(r.ok()) << r.error().message;

    std::lock_guard<std::mutex> lk(tracer->m);
    ASSERT_EQ(tracer->captures.size(), 2u);
    EXPECT_EQ(tracer->captures[0], tracer->root.get());  // main-thread factory
    EXPECT_EQ(tracer->captures[1], tracer->root.get());  // worker-side factory
}

// attachContext is user adapter code invoked inside the noexcept worker
// closure: a throw there must be caught into the frame and rethrown from
// co_await — never std::terminate on the worker.
TEST(CoroExecute, ThrowingAttachContextRethrownAtCoAwaitNotTerminate) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    auto tracer = std::make_shared<ThrowingAttachTracer>();
    PoolConfig cfg = noThread();
    cfg.observability.tracer = tracer;
    auto db = Database::open(driver, "X", cfg).value();
    EXPECT_THROW(syncWait(halcyon::coro::execute(db, "UPDATE t SET a=?", 1)),
                 std::runtime_error);
}

// LobSource: the WRAPPER is owned by the frame (temporaries are safe); only
// its referents are borrowed.
TEST(CoroExecute, TemporaryLobFileWrapperIsSafe) {
    MockCliDriver driver;
    driver.streamRowCounts.push_back(1);
    auto db = Database::open(driver, "X", noThread()).value();
    const std::string path = ::testing::TempDir() + "halcyon_coro_lob.bin";
    {
        std::ofstream f(path, std::ios::binary);
        f << "0123456789";
    }
    auto t = halcyon::coro::execute(db, "INSERT INTO docs VALUES (?, ?)", 7,
                                    halcyon::lobFile(path));  // temporary wrapper
    auto r = syncWait(std::move(t));
    ASSERT_TRUE(r.ok()) << r.error().message;
    ASSERT_EQ(driver.lastStreamedLobs.count(1), 1u);
    EXPECT_EQ(driver.lastStreamedLobs[1].size(), 10u);
    EXPECT_EQ(driver.executeStreamingCalls, 1);
}

TEST(CoroExecute, LobStreamReferentOutlivingTaskRoundTrips) {
    MockCliDriver driver;
    driver.streamRowCounts.push_back(1);
    auto db = Database::open(driver, "X", noThread()).value();
    std::istringstream in("abcdefgh");  // referent outlives the task: OK
    auto r = syncWait(halcyon::coro::execute(
        db, "INSERT INTO docs VALUES (?, ?)", 1, halcyon::lobStream(in)));
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(driver.lastStreamedLobs.count(1), 1u);
    EXPECT_EQ(driver.lastStreamedLobs[1].size(), 8u);
}

struct Num {
    std::int64_t n;
};
HALCYON_REFLECT(Num, n);

TEST(CoroQuery, ParityWithQueryAsyncMaterialization) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"n"},
        {{halcyon::detail::cli::Value{std::int64_t{2}}},
         {halcyon::detail::cli::Value{std::int64_t{5}}}}});
    auto db = Database::open(driver, "X", noThread()).value();
    auto r = syncWait(halcyon::coro::query<Num>(db, "SELECT n FROM t"));
    ASSERT_TRUE(r.ok()) << r.error().message;
    ASSERT_EQ(r.value().size(), 2u);
    EXPECT_EQ(r.value()[0].n, 2);
    EXPECT_EQ(r.value()[1].n, 5);
}

TEST(CoroQuery, NamedParamsOverload) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"n"}, {{halcyon::detail::cli::Value{std::int64_t{9}}}}});
    auto db = Database::open(driver, "X", noThread()).value();
    auto r = syncWait(halcyon::coro::query<Num>(
        db, "SELECT n FROM t WHERE id=:id", halcyon::params{{"id", 1}}));
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].n, 9);
}

// The documented throwing idiom: (co_await …).value().
TEST(CoroQuery, ThrowingIdiomUnwraps) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"n"}, {{halcyon::detail::cli::Value{std::int64_t{1}}}}});
    auto db = Database::open(driver, "X", noThread()).value();
    auto probe = [](Database& d) -> Task<std::size_t> {
        auto rows = (co_await halcyon::coro::query<Num>(d, "SELECT n FROM t"))
                        .value();
        co_return rows.size();
    };
    EXPECT_EQ(syncWait(probe(db)), 1u);
}

TEST(CoroExecuteBatch, ParityWithSyncExecuteBatch) {
    MockCliDriver driver;
    driver.batchRowCounts.push_back(3);
    auto db = Database::open(driver, "X", noThread()).value();
    auto batch = halcyon::batchOf({std::make_tuple(1, std::string{"a"}),
                                   std::make_tuple(2, std::string{"b"}),
                                   std::make_tuple(3, std::string{"c"})});
    auto r = syncWait(halcyon::coro::executeBatch(
        db, "INSERT INTO t(id, name) VALUES (?, ?)", std::move(batch)));
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value(), 3);
    EXPECT_EQ(driver.executeBatchCalls, 1);
    ASSERT_EQ(driver.lastBatchRows.size(), 3u);
}
