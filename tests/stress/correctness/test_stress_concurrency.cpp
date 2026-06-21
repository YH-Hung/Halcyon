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
