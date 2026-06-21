#pragma once

#include <memory>

#include "halcyon/observability/metrics.hpp"

// Factory for a prometheus-cpp-backed MetricsSink. The declaration is always
// available (it only forward-declares prometheus::Registry), but the definition
// is compiled into the library solely when HALCYON_WITH_PROMETHEUS is ON and
// prometheus-cpp was found. Link prometheus-cpp::core in that case.
namespace prometheus {
class Registry;
}

namespace halcyon::obs {

// Builds a MetricsSink that records into the given prometheus Registry. The
// caller owns the Registry and exposes it for scraping (e.g. via an Exposer).
std::shared_ptr<MetricsSink> make_prometheus_metrics(
    std::shared_ptr<prometheus::Registry> registry);

}  // namespace halcyon::obs
