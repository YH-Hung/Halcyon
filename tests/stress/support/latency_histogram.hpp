#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace halcyon::stress {

// Fixed-bucket log2 histogram of nanosecond latencies. Bucket i holds samples in
// [2^i, 2^(i+1)) ns. Allocation-free, trivially mergeable; percentiles return the
// bucket's upper bound in microseconds (approximate by design — cheap and
// distortion-free on the hot path).
class LatencyHistogram {
public:
    static constexpr int kBuckets = 64;

    void record(std::uint64_t ns) {
        ++counts_[bucket_for(ns)];
        ++total_;
    }

    void merge(const LatencyHistogram& o) {
        for (int i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        total_ += o.total_;
    }

    std::uint64_t count() const { return total_; }

    double percentile_us(double p) const {
        if (total_ == 0) return 0.0;
        const std::uint64_t target =
            static_cast<std::uint64_t>(p * static_cast<double>(total_));
        std::uint64_t cum = 0;
        for (int i = 0; i < kBuckets; ++i) {
            cum += counts_[i];
            if (cum >= target) return bucket_upper_us(i);
        }
        return max_us();
    }

    double max_us() const {
        for (int i = kBuckets - 1; i >= 0; --i)
            if (counts_[i]) return bucket_upper_us(i);
        return 0.0;
    }

private:
    static int bucket_for(std::uint64_t ns) {
        int b = 0;
        while ((ns >>= 1) && b < kBuckets - 1) ++b;
        return b;
    }
    static double bucket_upper_us(int i) {
        // upper bound of bucket i is 2^(i+1) ns -> /1000 us. Use ldexp so the
        // top bucket (i+1 == 64) never triggers shift-by-64 UB.
        return std::ldexp(1.0, i + 1) / 1000.0;
    }

    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t total_ = 0;
};

}  // namespace halcyon::stress
