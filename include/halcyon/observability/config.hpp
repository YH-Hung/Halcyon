#pragma once

#include <memory>

#include "halcyon/observability/metrics.hpp"
#include "halcyon/observability/tracing.hpp"

namespace halcyon::obs {

// Observability wiring for a Database/pool. Both pointers default to null, which
// means "no observability" (zero overhead). Populate with adapter-backed sinks
// (e.g. make_prometheus_metrics / make_otel_tracer) to instrument. Carried as a
// single field on PoolConfig so it flows through the one existing
// Database::open -> ConnectionPool::create path.
struct ObservabilityConfig {
    std::shared_ptr<MetricsSink> metrics;
    std::shared_ptr<Tracer> tracer;
};

}  // namespace halcyon::obs
