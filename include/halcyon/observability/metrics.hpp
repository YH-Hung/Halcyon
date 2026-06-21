#pragma once

#include <string_view>
#include <utility>
#include <vector>

namespace halcyon::obs {

// Label set passed with a metric sample. The string_view keys/values must
// outlive the (synchronous) sink call; an adapter that retains them past the
// call must copy into its own storage.
using Labels = std::vector<std::pair<std::string_view, std::string_view>>;

// Sink for library metrics (spec §9). Every method is called synchronously from
// the hot path, so implementations must be cheap and non-blocking.
struct MetricsSink {
    virtual ~MetricsSink() = default;
    virtual void counter(std::string_view name, double value,
                         const Labels& labels) = 0;
    virtual void histogram(std::string_view name, double value,
                           const Labels& labels) = 0;
    virtual void gauge(std::string_view name, double value,
                       const Labels& labels) = 0;
};

// Default sink: does nothing. The library resolves a null configured sink to the
// process-wide instance below, and gates emission so this is never even called
// on the default path (zero allocations, one predictable branch).
struct NoopMetricsSink final : MetricsSink {
    void counter(std::string_view, double, const Labels&) override {}
    void histogram(std::string_view, double, const Labels&) override {}
    void gauge(std::string_view, double, const Labels&) override {}
};

inline MetricsSink& noop_metrics_sink() {
    static NoopMetricsSink s;
    return s;
}

}  // namespace halcyon::obs
