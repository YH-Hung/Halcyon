// Coroutine-layer concurrency scenario (v1.2 §6): N threads each drive
// coroutine chains (query / transaction / streaming) through one shared
// Database while a poller hammers stats(). Build under TSan and ASan
// (-DHALCYON_SANITIZER=thread|address); assertions cover correctness, pool
// restoration, and stats invariants — the sanitizers cover races and leaks.
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "concurrent_fake_driver.hpp"
#include "halcyon/coro.hpp"
#include "halcyon/halcyon.hpp"

using halcyon::Database;
using halcyon::PoolConfig;
using halcyon::PoolStats;
using halcyon::Result;
using halcyon::Transaction;
using halcyon::coro::syncWait;
using halcyon::coro::Task;
using halcyon::stress::ConcurrentFakeDriver;

namespace {

struct StressNum {
    std::int64_t n;
};

}  // namespace
HALCYON_REFLECT(StressNum, n);

namespace {

Task<Result<std::int64_t>> one_chain(Database& db, int iter) {
    // 1. typed query (auto-retried read path)
    auto q = co_await halcyon::coro::query<StressNum>(
        db, "SELECT 7 FROM SYSIBM.SYSDUMMY1");
    if (!q.ok()) co_return Result<std::int64_t>(q.error());
    if (q.value().size() != 1 || q.value()[0].n != 7) {
        halcyon::Error e;
        e.code = halcyon::ErrorCode::Unknown;
        e.message = "wrong query payload";
        co_return Result<std::int64_t>(e);
    }
    // 2. transaction (plain fn on a worker)
    auto t = co_await halcyon::coro::transaction(db, [&](Transaction& tx) {
        auto r = tx.execute("UPDATE t SET a = " + std::to_string(iter));
        if (!r.ok()) return Result<int>(r.error());
        return Result<int>(iter);
    });
    if (!t.ok()) co_return Result<std::int64_t>(t.error());
    // 3. streaming row + LOB drain
    auto sqr = co_await halcyon::coro::queryStreaming(
        db, "SELECT 5 FROM SYSIBM.SYSDUMMY1");
    if (!sqr.ok()) co_return Result<std::int64_t>(sqr.error());
    auto sq = std::move(sqr.value());
    std::int64_t streamed = 0;
    while (auto row = co_await sq.next()) {
        auto reader = row->lob(0);
        if (!reader.ok()) co_return Result<std::int64_t>(reader.error());
        auto bytes = co_await reader.value().toVector();
        if (!bytes.ok()) co_return Result<std::int64_t>(bytes.error());
        streamed += static_cast<std::int64_t>(bytes.value().size());
    }
    if (!sq.ok()) co_return Result<std::int64_t>(*sq.error());
    co_return Result<std::int64_t>(streamed);
}

}  // namespace

TEST(StressCoro, ConcurrentChainsWithStatsPolling) {
    auto fake = std::make_shared<ConcurrentFakeDriver>();
    PoolConfig cfg;
    cfg.min = 2;
    cfg.max = 8;
    auto db = Database::open(fake, "stress", cfg).value();

    constexpr int kThreads = 8;
    constexpr int kIters = 50;
    std::atomic<bool> done{false};
    std::atomic<long> failures{0};

    std::thread poller([&] {
        while (!done.load(std::memory_order_relaxed)) {
            PoolStats s = db.poolStats();
            if (s.busy > s.size || s.idle + s.busy != s.size) ++failures;
            if (s.peakBusy > cfg.max) ++failures;
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> drivers;
    for (int t = 0; t < kThreads; ++t) {
        drivers.emplace_back([&, t] {
            for (int i = 0; i < kIters; ++i) {
                auto r = syncWait(one_chain(db, t * kIters + i));
                if (!r.ok() ||
                    r.value() != static_cast<std::int64_t>(
                                     ConcurrentFakeDriver::kStreamLobBytes))
                    ++failures;
            }
        });
    }
    for (auto& t : drivers) t.join();
    done.store(true);
    poller.join();

    EXPECT_EQ(failures.load(), 0);
    // Pool restored: nothing leased, nothing leaked.
    PoolStats end = db.poolStats();
    EXPECT_EQ(end.busy, 0u);
    EXPECT_EQ(end.idle, end.size);
    EXPECT_GE(end.acquiredTotal, static_cast<std::uint64_t>(kThreads * kIters));
}
