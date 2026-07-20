#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "halcyon/coro.hpp"
#include "halcyon/observability/tracing.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::ErrorCode;
using halcyon::PoolConfig;
using halcyon::Result;
using halcyon::coro::syncWait;
using halcyon::coro::Task;
using halcyon::testing::MockCliDriver;

namespace {

PoolConfig noThread() {
    PoolConfig c;
    c.startMaintenanceThread = false;
    return c;
}

using Value = halcyon::detail::cli::Value;

MockCliDriver::ScriptedRows lobRows(std::vector<Value> cells) {
    std::vector<std::vector<Value>> rows;
    rows.push_back(std::move(cells));
    return MockCliDriver::ScriptedRows{{"id", "doc"}, std::move(rows)};
}

// Adapter whose attachContext throws from the Nth call on (1-based) — lets
// the queryStreaming offload's attach succeed and the next() attach fail.
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

TEST(CoroStreaming, RowLoopWithScalarCells) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"n"},
        {{Value{std::int64_t{1}}}, {Value{std::int64_t{2}}}, {Value{std::int64_t{3}}}}});
    auto db = Database::open(driver, "X", noThread()).value();

    auto drive = [](Database& d) -> Task<std::int64_t> {
        auto sq = (co_await halcyon::coro::queryStreaming(d, "SELECT n FROM t"))
                      .value();
        std::int64_t sum = 0;
        while (auto row = co_await sq.next()) {
            sum += row->get<std::int64_t>(0).value();  // scalar reads: synchronous
        }
        EXPECT_TRUE(sq.ok());
        co_return sum;
    };
    EXPECT_EQ(syncWait(drive(db)), 6);
}

TEST(CoroStreaming, LobChunkedReadWithExactBufferMultiple) {
    MockCliDriver driver;
    driver.resultSets.push_back(
        lobRows({Value{std::int64_t{1}}, Value{std::string{"0123456789ABCDEF"}}}));
    auto db = Database::open(driver, "X", noThread()).value();

    auto drive = [](Database& d) -> Task<std::string> {
        auto sq =
            (co_await halcyon::coro::queryStreaming(d, "SELECT id, doc FROM t"))
                .value();
        auto row = co_await sq.next();
        EXPECT_TRUE(row.has_value());
        (void)row->get<std::int64_t>(0).value();
        auto reader = row->lob(1).value();
        std::string out;
        std::byte buf[8];  // 16 bytes = exactly 2 full buffers
        for (;;) {
            auto n = (co_await reader.read(buf, sizeof buf)).value();
            if (n == 0) break;
            out.append(reinterpret_cast<const char*>(buf), n);
        }
        EXPECT_FALSE(reader.isNull());
        co_return out;
    };
    EXPECT_EQ(syncWait(drive(db)), "0123456789ABCDEF");
}

TEST(CoroStreaming, EmptyLobReadsZeroNotNull) {
    MockCliDriver driver;
    driver.resultSets.push_back(
        lobRows({Value{std::int64_t{1}}, Value{std::string{""}}}));
    auto db = Database::open(driver, "X", noThread()).value();
    auto drive = [](Database& d) -> Task<bool> {
        auto sq =
            (co_await halcyon::coro::queryStreaming(d, "SELECT id, doc FROM t"))
                .value();
        auto row = co_await sq.next();
        auto reader = row->lob(1).value();
        std::byte buf[8];
        auto n = (co_await reader.read(buf, sizeof buf)).value();
        EXPECT_EQ(n, 0u);
        co_return reader.isNull();
    };
    EXPECT_FALSE(syncWait(drive(db)));  // empty, not NULL
}

TEST(CoroStreaming, NullLobIsImmediateEofWithIsNull) {
    MockCliDriver driver;
    driver.resultSets.push_back(
        lobRows({Value{std::int64_t{1}}, Value{halcyon::detail::cli::Null{}}}));
    auto db = Database::open(driver, "X", noThread()).value();
    auto drive = [](Database& d) -> Task<bool> {
        auto sq =
            (co_await halcyon::coro::queryStreaming(d, "SELECT id, doc FROM t"))
                .value();
        auto row = co_await sq.next();
        auto reader = row->lob(1).value();
        std::byte buf[8];
        auto n = (co_await reader.read(buf, sizeof buf)).value();
        EXPECT_EQ(n, 0u);
        co_return reader.isNull();
    };
    EXPECT_TRUE(syncWait(drive(db)));
}

TEST(CoroStreaming, DrainHelpersToStringAndToVector) {
    MockCliDriver driver;
    driver.lobChunkCap = 4;  // force multiple driver chunks inside ONE offload
    driver.resultSets.push_back(
        lobRows({Value{std::int64_t{1}}, Value{std::string{"hello streaming"}}}));
    driver.resultSets.push_back(
        lobRows({Value{std::int64_t{2}}, Value{std::string{"bytes"}}}));
    auto db = Database::open(driver, "X", noThread()).value();

    auto drive1 = [](Database& d) -> Task<std::string> {
        auto sq =
            (co_await halcyon::coro::queryStreaming(d, "SELECT id, doc FROM t"))
                .value();
        auto row = co_await sq.next();
        (void)row->get<std::int64_t>(0).value();
        auto reader = row->lob(1).value();
        co_return(co_await reader.toString()).value();
    };
    EXPECT_EQ(syncWait(drive1(db)), "hello streaming");

    auto drive2 = [](Database& d) -> Task<std::size_t> {
        auto sq =
            (co_await halcyon::coro::queryStreaming(d, "SELECT id, doc FROM t"))
                .value();
        auto row = co_await sq.next();
        auto reader = row->lob(1).value();
        co_return(co_await reader.toVector()).value().size();
    };
    EXPECT_EQ(syncWait(drive2(db)), 5u);
}

// Ascending-column-order enforcement is inherited from v1.1 unchanged.
TEST(CoroStreaming, OutOfOrderColumnAccessIsInvalidState) {
    MockCliDriver driver;
    driver.resultSets.push_back(
        lobRows({Value{std::int64_t{1}}, Value{std::string{"x"}}}));
    auto db = Database::open(driver, "X", noThread()).value();
    auto drive = [](Database& d) -> Task<ErrorCode> {
        auto sq =
            (co_await halcyon::coro::queryStreaming(d, "SELECT id, doc FROM t"))
                .value();
        auto row = co_await sq.next();
        (void)row->lob(1).value();             // claims column 1
        auto bad = row->get<std::int64_t>(0);  // behind the high-water mark
        co_return bad.ok() ? ErrorCode::Ok : bad.error().code;
    };
    EXPECT_EQ(syncWait(drive(db)), ErrorCode::InvalidState);
}

// Mid-stream abandonment: destroying the StreamingQuery closes the cursor and
// returns the lease (ASan-clean; pool state restored).
TEST(CoroStreaming, MidStreamAbandonReturnsLease) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"n"}, {{Value{std::int64_t{1}}}, {Value{std::int64_t{2}}}}});
    auto db = Database::open(driver, "X", noThread()).value();
    auto drive = [](Database& d) -> Task<void> {
        auto sq = (co_await halcyon::coro::queryStreaming(d, "SELECT n FROM t"))
                      .value();
        auto row = co_await sq.next();
        EXPECT_TRUE(row.has_value());
        co_return;  // sq destroyed with rows remaining
    };
    syncWait(drive(db));
    EXPECT_EQ(db.pool().idle_count(), db.pool().total_count());  // lease back
    EXPECT_GE(driver.closeCursorCalls, 1);
}

// Executor gone between rows: next() completes with nullopt and the error is
// InvalidState via sq.error() — the same end-or-error model as the sync API.
// The coroutine must NOT hold a Database copy (that would keep the executor
// alive); the StreamingQuery's own backing keeps pool + driver alive. The
// reset happens ON a worker thread (the continuation after the first next()),
// exercising teardown-on-worker (Task 1) end to end.
TEST(CoroStreaming, ExecutorGoneMidStreamReportsInvalidState) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"n"}, {{Value{std::int64_t{1}}}, {Value{std::int64_t{2}}}}});
    auto db = std::make_optional(Database::open(driver, "X", noThread()).value());
    auto drive = [&]() -> Task<ErrorCode> {
        auto sq =
            (co_await halcyon::coro::queryStreaming(*db, "SELECT n FROM t"))
                .value();
        auto row = co_await sq.next();
        EXPECT_TRUE(row.has_value());
        db.reset();  // last Database copy dies HERE, on a worker thread
        auto row2 = co_await sq.next();
        EXPECT_FALSE(row2.has_value());
        EXPECT_FALSE(sq.ok());
        co_return sq.error()->code;
    };
    auto t = drive();
    EXPECT_EQ(syncWait(std::move(t)), ErrorCode::InvalidState);
}

// NextAwaitable shares the containment contract (no carve-out here, unlike
// the transaction hop): a throwing attachContext in its worker closure is
// caught into the frame and rethrown from co_await sq.next() — never
// std::terminate on the worker. Attach #1 (the queryStreaming offload's)
// succeeds so the cursor opens; attach #2 (the first next()'s) throws.
TEST(CoroStreaming, ThrowingAttachOnNextRethrownAtCoAwait) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"n"}, {{Value{std::int64_t{1}}}}});
    auto tracer = std::make_shared<CountingFlakyTracer>();
    tracer->throwFromCall = 2;  // query offload attach ok; next() attach throws
    PoolConfig cfg = noThread();
    cfg.observability.tracer = tracer;
    auto db = Database::open(driver, "X", cfg).value();
    auto drive = [](Database& d) -> Task<void> {
        auto sq = (co_await halcyon::coro::queryStreaming(d, "SELECT n FROM t"))
                      .value();
        (void)co_await sq.next();  // attach throws -> rethrown here
    };
    EXPECT_THROW(syncWait(drive(db)), std::runtime_error);
    // Frame unwind released the cursor and lease cleanly.
    EXPECT_EQ(db.pool().idle_count(), db.pool().total_count());
}
