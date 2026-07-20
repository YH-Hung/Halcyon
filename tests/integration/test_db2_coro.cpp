#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "halcyon/coro.hpp"
#include "halcyon/halcyon.hpp"

using halcyon::Database;
using halcyon::Result;
using halcyon::Transaction;
using halcyon::coro::syncWait;
using halcyon::coro::Task;

namespace {

std::optional<std::string> dsn() {
    if (const char* v = std::getenv("HALCYON_TEST_DSN")) return std::string(v);
    return std::nullopt;
}

std::uint64_t fnv1a(const std::byte* p, std::size_t n, std::uint64_t h) {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}
constexpr std::uint64_t kFnvSeed = 1469598103934665603ull;
constexpr std::size_t kLobBytes = 32u * 1024 * 1024;  // >= 32 MiB (spec §6)

std::byte pattern_byte(std::size_t pos) {
    return static_cast<std::byte>((pos * 31 + 7) & 0xFF);
}

}  // namespace

struct CoroCount {
    std::int64_t c;
};
HALCYON_REFLECT(CoroCount, c);

struct CoroPerson {
    int id;
    std::string name;
};
HALCYON_REFLECT(CoroPerson, id, name);

class Db2CoroIntegration : public ::testing::Test {
protected:
    void SetUp() override {
        auto d = dsn();
        if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";
        auto db = Database::open(*d);
        ASSERT_TRUE(db.ok()) << db.error().message;
        db_.emplace(std::move(db.value()));
        db_->execute("DROP TABLE halcyon_coro_people");  // ignore error
        ASSERT_TRUE(db_->execute("CREATE TABLE halcyon_coro_people("
                                 "id INT NOT NULL, name VARCHAR(64))")
                        .ok());
    }

    std::optional<Database> db_;
};

TEST_F(Db2CoroIntegration, ExecuteQueryBatchRoundTrip) {
    auto flow = [](Database& db) -> Task<Result<std::vector<CoroPerson>>> {
        auto ins = co_await halcyon::coro::execute(
            db, "INSERT INTO halcyon_coro_people VALUES (?, ?)", 1,
            std::string{"ada"});
        if (!ins.ok()) co_return Result<std::vector<CoroPerson>>(ins.error());

        auto batch = halcyon::batchOf({std::make_tuple(2, std::string{"bob"}),
                                       std::make_tuple(3, std::string{"eve"})});
        auto b = co_await halcyon::coro::executeBatch(
            db, "INSERT INTO halcyon_coro_people VALUES (?, ?)",
            std::move(batch));
        if (!b.ok()) co_return Result<std::vector<CoroPerson>>(b.error());

        co_return co_await halcyon::coro::query<CoroPerson>(
            db, "SELECT id, name FROM halcyon_coro_people ORDER BY id");
    };
    auto rows = syncWait(flow(*db_));
    ASSERT_TRUE(rows.ok()) << rows.error().message;
    ASSERT_EQ(rows.value().size(), 3u);
    EXPECT_EQ(rows.value()[0].name, "ada");
    EXPECT_EQ(rows.value()[2].id, 3);
}

TEST_F(Db2CoroIntegration, TransactionCommitAndRollbackVisibility) {
    // A SECOND Database (own pool + connections) observes visibility.
    auto other = Database::open(*dsn());
    ASSERT_TRUE(other.ok());

    auto committed = syncWait(halcyon::coro::transaction(*db_, [](Transaction& tx) {
        auto r = tx.execute("INSERT INTO halcyon_coro_people VALUES (?, ?)", 10,
                            std::string{"committed"});
        if (!r.ok()) return Result<int>(r.error());
        return Result<int>(1);
    }));
    ASSERT_TRUE(committed.ok()) << committed.error().message;
    auto seen = other.value().queryAs<CoroCount>(
        "SELECT COUNT(*) FROM halcyon_coro_people WHERE id = 10");
    ASSERT_TRUE(seen.ok());
    EXPECT_EQ(seen.value()[0].c, 1);

    auto rolledBack = syncWait(halcyon::coro::transaction(*db_, [](Transaction& tx) {
        auto r = tx.execute("INSERT INTO halcyon_coro_people VALUES (?, ?)", 11,
                            std::string{"doomed"});
        if (!r.ok()) return Result<int>(r.error());
        halcyon::Error e;
        e.code = halcyon::ErrorCode::Unknown;
        e.message = "force rollback";
        return Result<int>(e);
    }));
    ASSERT_FALSE(rolledBack.ok());
    auto gone = other.value().queryAs<CoroCount>(
        "SELECT COUNT(*) FROM halcyon_coro_people WHERE id = 11");
    ASSERT_TRUE(gone.ok());
    EXPECT_EQ(gone.value()[0].c, 0);
}

TEST_F(Db2CoroIntegration, StreamingLob32MiBChecksumRoundTrip) {
    db_->execute("DROP TABLE halcyon_coro_lob");  // ignore error
    ASSERT_TRUE(db_->execute("CREATE TABLE halcyon_coro_lob("
                             "id INT NOT NULL, doc BLOB(64M))")
                    .ok());

    // Upload: deterministic pattern via lobCallback; hash while producing.
    std::uint64_t upHash = kFnvSeed;
    std::size_t produced = 0;
    auto pull = [&](std::byte* buf, std::size_t cap) -> std::size_t {
        const std::size_t n = std::min(cap, kLobBytes - produced);
        for (std::size_t i = 0; i < n; ++i) buf[i] = pattern_byte(produced + i);
        upHash = fnv1a(buf, n, upHash);
        produced += n;
        return n;
    };
    auto up = syncWait(halcyon::coro::execute(
        *db_, "INSERT INTO halcyon_coro_lob VALUES (?, ?)", 1,
        halcyon::lobCallback(pull, kLobBytes)));
    ASSERT_TRUE(up.ok()) << up.error().message;
    ASSERT_EQ(produced, kLobBytes);

    // Download: awaitable chunked reads; hash while consuming.
    auto drive = [](Database& db) -> Task<std::pair<std::uint64_t, std::size_t>> {
        auto sq = (co_await halcyon::coro::queryStreaming(
                       db, "SELECT id, doc FROM halcyon_coro_lob"))
                      .value();
        auto row = co_await sq.next();
        EXPECT_TRUE(row.has_value());
        (void)row->get<std::int64_t>(0).value();
        auto reader = row->lob(1).value();
        std::vector<std::byte> buf(64 * 1024);
        std::uint64_t h = kFnvSeed;
        std::size_t total = 0;
        for (;;) {
            auto n = (co_await reader.read(buf.data(), buf.size())).value();
            if (n == 0) break;
            h = fnv1a(buf.data(), n, h);
            total += n;
        }
        EXPECT_TRUE(sq.ok());
        co_return std::make_pair(h, total);
    };
    auto [downHash, downBytes] = syncWait(drive(*db_));
    EXPECT_EQ(downBytes, kLobBytes);
    EXPECT_EQ(downHash, upHash);
}

TEST_F(Db2CoroIntegration, ConcurrentCoroutineChainsThroughThePool) {
    constexpr int kThreads = 8;
    constexpr int kIters = 25;
    auto chain = [](Database& db, int tid) -> Task<Result<std::int64_t>> {
        for (int i = 0; i < kIters; ++i) {
            auto ins = co_await halcyon::coro::execute(
                db, "INSERT INTO halcyon_coro_people VALUES (?, ?)",
                1000 + tid * 100 + i, std::string{"worker"});
            if (!ins.ok()) co_return Result<std::int64_t>(ins.error());
        }
        auto n = co_await halcyon::coro::query<CoroCount>(
            db, "SELECT COUNT(*) FROM halcyon_coro_people WHERE id >= 1000");
        if (!n.ok()) co_return Result<std::int64_t>(n.error());
        co_return Result<std::int64_t>(n.value()[0].c);
    };

    std::vector<std::thread> threads;
    std::vector<std::optional<Result<std::int64_t>>> results(kThreads);
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back(
            [&, t] { results[t].emplace(syncWait(chain(*db_, t))); });
    for (auto& t : threads) t.join();
    for (int t = 0; t < kThreads; ++t) {
        ASSERT_TRUE(results[t].has_value());
        ASSERT_TRUE(results[t]->ok()) << results[t]->error().message;
    }

    auto total = db_->queryAs<CoroCount>(
        "SELECT COUNT(*) FROM halcyon_coro_people WHERE id >= 1000");
    ASSERT_TRUE(total.ok());
    EXPECT_EQ(total.value()[0].c, kThreads * kIters);
}
