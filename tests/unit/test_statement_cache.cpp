#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "capturing_logger.hpp"
#include "halcyon/detail/statement_cache.hpp"
#include "mock_cli_driver.hpp"

using halcyon::detail::StatementCache;
using halcyon::detail::StatementLease;
using halcyon::testing::MockCliDriver;
using ConnectionParams = halcyon::detail::cli::ConnectionParams;

namespace {

// Minimal recording sink: counts counter samples by (name, result-label) and
// remembers the last gauge value per name.
class RecordingSink : public halcyon::obs::MetricsSink {
public:
    void counter(std::string_view n, double v,
                 const halcyon::obs::Labels& l) override {
        std::string key{n};
        for (auto& kv : l)
            if (kv.first == "result") key += std::string{":"} + std::string{kv.second};
        counts[key] += v;
    }
    void histogram(std::string_view, double,
                   const halcyon::obs::Labels&) override {}
    void gauge(std::string_view n, double v,
               const halcyon::obs::Labels&) override {
        gauges[std::string{n}] = v;
    }
    std::map<std::string, double> counts;
    std::map<std::string, double> gauges;
};

inline halcyon::detail::cli::ConnectionHandle open_conn(MockCliDriver& d) {
    return d.connect(ConnectionParams{"x"}).value();
}

}  // namespace

TEST(StatementCache, MissThenHitPreparesOnce) {
    MockCliDriver driver;
    StatementCache cache(driver, open_conn(driver), /*capacity=*/8);
    const std::string sql = "SELECT 1 FROM SYSIBM.SYSDUMMY1";

    {
        auto l1 = cache.acquire(sql);
        ASSERT_TRUE(l1.ok());
    }  // miss, released
    {
        auto l2 = cache.acquire(sql);
        ASSERT_TRUE(l2.ok());
    }  // hit, released

    EXPECT_EQ(driver.preparedSql.size(), 1u);  // prepared once, reused
    EXPECT_EQ(driver.finalizeCalls, 0);        // cached, not finalized
    EXPECT_GE(driver.closeCursorCalls, 2);     // closed on each release
}

TEST(StatementCache, CapacityZeroIsAlwaysTransient) {
    MockCliDriver driver;
    StatementCache cache(driver, open_conn(driver), /*capacity=*/0);
    const std::string sql = "SELECT 1";

    {
        auto l = cache.acquire(sql);
        ASSERT_TRUE(l.ok());
    }
    {
        auto l = cache.acquire(sql);
        ASSERT_TRUE(l.ok());
    }

    EXPECT_EQ(driver.preparedSql.size(), 2u);  // no reuse
    EXPECT_EQ(driver.finalizeCalls, 2);        // each transient finalized
}

TEST(StatementCache, EvictsLeastRecentlyUsedAtCapacity) {
    MockCliDriver driver;
    StatementCache cache(driver, open_conn(driver), /*capacity=*/2);

    { auto a = cache.acquire("A"); }  // entries: A
    { auto b = cache.acquire("B"); }  // entries: B, A
    { auto a = cache.acquire("A"); }  // touch A -> entries: A, B
    { auto c = cache.acquire("C"); }  // capacity hit: evict LRU idle = B

    EXPECT_EQ(driver.preparedSql.size(), 3u);  // A, B, C
    EXPECT_EQ(driver.finalizeCalls, 1);        // B evicted+finalized
    { auto b = cache.acquire("B"); }           // B was evicted -> re-prepare
    EXPECT_EQ(driver.preparedSql.size(), 4u);
}

TEST(StatementCache, BusyHitServesTransientOverflow) {
    MockCliDriver driver;
    StatementCache cache(driver, open_conn(driver), /*capacity=*/8);
    const std::string sql = "SELECT id FROM t";

    auto l1 = cache.acquire(sql);  // miss, kept busy (not released)
    ASSERT_TRUE(l1.ok());
    auto l2 = cache.acquire(sql);  // same sql while busy -> transient
    ASSERT_TRUE(l2.ok());

    EXPECT_EQ(driver.preparedSql.size(), 2u);             // one cached + one transient
    EXPECT_NE(l1.value().handle(), l2.value().handle());  // distinct handles
}

TEST(StatementCache, PoisonDropsEntry) {
    MockCliDriver driver;
    StatementCache cache(driver, open_conn(driver), /*capacity=*/8);
    const std::string sql = "SELECT 1";

    {
        auto l = cache.acquire(sql);
        ASSERT_TRUE(l.ok());
        l.value().poison();
    }
    EXPECT_EQ(driver.finalizeCalls, 1);  // poisoned entry finalized on release
    {
        auto l = cache.acquire(sql);
        ASSERT_TRUE(l.ok());
    }  // re-prepared
    EXPECT_EQ(driver.preparedSql.size(), 2u);
}

TEST(StatementCache, CloseFailureDropsEntry) {
    MockCliDriver driver;
    StatementCache cache(driver, open_conn(driver), /*capacity=*/8);
    const std::string sql = "SELECT 1";
    driver.closeCursorErrors.push_back([] {
        halcyon::Error e;
        e.code = halcyon::ErrorCode::Connection;
        e.message = "reset failed";
        return e;
    }());

    // Miss; release closeCursor fails.
    {
        auto l = cache.acquire(sql);
        ASSERT_TRUE(l.ok());
    }
    EXPECT_EQ(driver.finalizeCalls, 1);  // half-reset handle dropped, not reused
    {
        auto l = cache.acquire(sql);
        ASSERT_TRUE(l.ok());
    }  // re-prepared
    EXPECT_EQ(driver.preparedSql.size(), 2u);
}

TEST(StatementCache, DestructorFinalizesLiveEntries) {
    MockCliDriver driver;
    {
        StatementCache cache(driver, open_conn(driver), /*capacity=*/8);
        { auto a = cache.acquire("A"); }
        { auto b = cache.acquire("B"); }
        EXPECT_EQ(driver.finalizeCalls, 0);
    }  // cache destroyed
    EXPECT_EQ(driver.finalizeCalls, 2);  // A and B finalized
}

TEST(StatementCache, EmitsMetrics) {
    MockCliDriver driver;
    RecordingSink sink;
    StatementCache cache(driver, open_conn(driver), /*capacity=*/8, &sink);
    const std::string sql = "SELECT 1";

    { auto a = cache.acquire(sql); }  // miss
    { auto b = cache.acquire(sql); }  // hit

    EXPECT_DOUBLE_EQ(sink.counts["halcyon_stmt_cache_total:miss"], 1.0);
    EXPECT_DOUBLE_EQ(sink.counts["halcyon_stmt_cache_total:hit"], 1.0);
    EXPECT_DOUBLE_EQ(sink.gauges["halcyon_stmt_cache_size"], 1.0);
}

TEST(StatementCacheLogging, EvictionIsLogged) {
    halcyon::testing::MockCliDriver drv;
    halcyon::testing::CapturingLogger logger;
    auto conn = drv.connect({"dsn"});
    ASSERT_TRUE(conn.ok());
    halcyon::detail::StatementCache cache(drv, conn.value(), /*capacity=*/1,
                                          /*metrics=*/nullptr, &logger);
    {
        auto a = cache.acquire("SELECT 1");
        ASSERT_TRUE(a.ok());
    }
    {
        auto b = cache.acquire("SELECT 2");
        ASSERT_TRUE(b.ok());
    }  // evicts SELECT 1
    EXPECT_EQ(logger.count("stmt_cache.evict"), 1u);
}
