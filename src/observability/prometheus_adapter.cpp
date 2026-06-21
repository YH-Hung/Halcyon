#include "halcyon/observability/prometheus_adapter.hpp"

#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace halcyon::obs {
namespace {

// Default seconds buckets for duration/wait histograms (sub-ms to ~16s).
const prometheus::Histogram::BucketBoundaries& default_buckets() {
    static const prometheus::Histogram::BucketBoundaries b = {
        0.0005, 0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25,
        0.5,    1.0,   2.5,   5.0,  10.0,  16.0};
    return b;
}

std::map<std::string, std::string> to_map(const Labels& labels) {
    std::map<std::string, std::string> m;
    for (const auto& [k, v] : labels) m.emplace(std::string(k), std::string(v));
    return m;
}

// MetricsSink over a prometheus-cpp Registry. Families are created once per name
// and cached (registering a duplicate family name throws), then the per-label
// child is resolved via Family::Add (which the family caches internally).
class PrometheusMetricsSink final : public MetricsSink {
public:
    explicit PrometheusMetricsSink(std::shared_ptr<prometheus::Registry> registry)
        : registry_(std::move(registry)) {}

    void counter(std::string_view name, double value,
                 const Labels& labels) override {
        counter_family(std::string(name)).Add(to_map(labels)).Increment(value);
    }
    void histogram(std::string_view name, double value,
                   const Labels& labels) override {
        histogram_family(std::string(name))
            .Add(to_map(labels), default_buckets())
            .Observe(value);
    }
    void gauge(std::string_view name, double value,
               const Labels& labels) override {
        gauge_family(std::string(name)).Add(to_map(labels)).Set(value);
    }

private:
    prometheus::Family<prometheus::Counter>& counter_family(
        const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = counters_.find(name);
        if (it != counters_.end()) return *it->second;
        auto& f = prometheus::BuildCounter().Name(name).Help(name).Register(
            *registry_);
        counters_.emplace(name, &f);
        return f;
    }
    prometheus::Family<prometheus::Histogram>& histogram_family(
        const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = histograms_.find(name);
        if (it != histograms_.end()) return *it->second;
        auto& f = prometheus::BuildHistogram().Name(name).Help(name).Register(
            *registry_);
        histograms_.emplace(name, &f);
        return f;
    }
    prometheus::Family<prometheus::Gauge>& gauge_family(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = gauges_.find(name);
        if (it != gauges_.end()) return *it->second;
        auto& f =
            prometheus::BuildGauge().Name(name).Help(name).Register(*registry_);
        gauges_.emplace(name, &f);
        return f;
    }

    std::shared_ptr<prometheus::Registry> registry_;
    std::mutex mu_;
    std::map<std::string, prometheus::Family<prometheus::Counter>*> counters_;
    std::map<std::string, prometheus::Family<prometheus::Histogram>*> histograms_;
    std::map<std::string, prometheus::Family<prometheus::Gauge>*> gauges_;
};

}  // namespace

std::shared_ptr<MetricsSink> make_prometheus_metrics(
    std::shared_ptr<prometheus::Registry> registry) {
    return std::make_shared<PrometheusMetricsSink>(std::move(registry));
}

}  // namespace halcyon::obs
