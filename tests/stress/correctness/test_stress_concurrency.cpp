#include <gtest/gtest.h>

#include "latency_histogram.hpp"

using halcyon::stress::LatencyHistogram;

TEST(LatencyHistogramTest, CountsAndMerges) {
    LatencyHistogram a;
    for (int i = 0; i < 100; ++i) a.record(1000);  // 1us each
    LatencyHistogram b;
    for (int i = 0; i < 100; ++i) b.record(1000);
    a.merge(b);
    EXPECT_EQ(a.count(), 200u);
}

TEST(LatencyHistogramTest, PercentileIsMonotonic) {
    LatencyHistogram h;
    for (int i = 0; i < 1000; ++i) h.record(1000);          // ~1us
    for (int i = 0; i < 10; ++i) h.record(1000ull * 1000);  // ~1ms tail
    EXPECT_GT(h.count(), 0u);
    EXPECT_LE(h.percentile_us(0.50), h.percentile_us(0.99));
    EXPECT_LE(h.percentile_us(0.99), h.max_us());
}

#include "concurrent_fake_driver.hpp"

using halcyon::stress::ConcurrentFakeDriver;
namespace cli = halcyon::detail::cli;

TEST(FakeDriverTest, SelectReturnsSqlEncodedValue) {
    ConcurrentFakeDriver d;
    auto c = d.connect({"dsn"});
    ASSERT_TRUE(c.ok());
    auto s = d.prepare(c.value(), "SELECT 7 FROM SYSIBM.SYSDUMMY1");
    ASSERT_TRUE(s.ok());
    ASSERT_TRUE(d.bindParams(s.value(), {}).ok());
    ASSERT_TRUE(d.execute(s.value()).ok());
    EXPECT_EQ(d.columnCount(s.value()).value(), 1u);
    ASSERT_TRUE(d.fetch(s.value()).value());  // one row
    auto col = d.getColumn(s.value(), 0);
    ASSERT_TRUE(col.ok());
    EXPECT_EQ(std::get<std::int64_t>(col.value()), 7);
    EXPECT_FALSE(d.fetch(s.value()).value());  // end of cursor
}

TEST(FakeDriverTest, FailExecuteEveryTripsAtRate) {
    ConcurrentFakeDriver d;
    d.failExecuteEvery = 2;  // every 2nd execute fails, retriable
    auto c = d.connect({"dsn"}).value();
    auto s = d.prepare(c, "SELECT 1 FROM SYSIBM.SYSDUMMY1").value();
    EXPECT_TRUE(d.execute(s).ok());        // call 1
    auto r2 = d.execute(s);                // call 2 -> fail
    ASSERT_FALSE(r2.ok());
    EXPECT_TRUE(r2.error().retriable);
}

TEST(FakeDriverTest, StatementOnDeadConnectionErrors) {
    ConcurrentFakeDriver d;
    auto c = d.connect({"dsn"}).value();
    auto s = d.prepare(c, "SELECT 1 FROM SYSIBM.SYSDUMMY1").value();
    ASSERT_TRUE(d.disconnect(c).ok());
    EXPECT_FALSE(d.execute(s).ok());  // use after the connection died
}
