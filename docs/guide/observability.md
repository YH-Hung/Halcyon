# Observability

Halcyon emits Prometheus metrics and OpenTelemetry spans via optional adapters.
Both are **zero-overhead when disabled** — the library has a single branch and
emits nothing when neither adapter is wired in.

## Wiring adapters

Pass an `obs::ObservabilityConfig` inside `PoolConfig`:

```cpp
#include <halcyon/halcyon.hpp>
#include <halcyon/observability/prometheus_adapter.hpp>  // HALCYON_WITH_PROMETHEUS
#include <halcyon/observability/otel_adapter.hpp>        // HALCYON_WITH_OTEL

// Prometheus
auto registry = std::make_shared<prometheus::Registry>();
auto metrics  = halcyon::obs::make_prometheus_metrics(registry);
// ... expose registry via prometheus::Exposer on :9090/metrics ...

// OpenTelemetry (uses the process-global tracer provider)
auto tracer = halcyon::obs::make_otel_tracer("my-service");

halcyon::PoolConfig config{
    .min = 2, .max = 16,
    .observability = {
        .metrics = metrics,
        .tracer  = tracer,
    },
};

auto db = halcyon::Database::openOrThrow(dsn, config);
```

### CMake flags

| Flag | What it enables |
|---|---|
| `-DHALCYON_WITH_PROMETHEUS=ON` | Compiles `prometheus_adapter.cpp`; link `prometheus-cpp::core` |
| `-DHALCYON_WITH_OTEL=ON` | Compiles `otel_adapter.cpp`; link opentelemetry-cpp |

## Metrics reference

All metrics are emitted synchronously from the hot path. Implementations must be
non-blocking (the `MetricsSink` contract).

### Query / execute metrics

| Metric | Type | Labels | Description |
|---|---|---|---|
| `halcyon_queries_total` | Counter | `op`, `status` | Every `query`/`execute` call; `op` is `SELECT`, `INSERT`, etc.; `status` is `ok` or `error` |
| `halcyon_query_duration_seconds` | Histogram | `op` | Wall time from call entry to first row / affected-row return |
| `halcyon_errors_total` | Counter | `code` | Failed calls; `code` matches `ErrorCode` (e.g. `Connection`, `Constraint`) |
| `halcyon_retries_total` | Counter | `outcome` | Auto-retry events; `outcome` is `retried` (before replay) or `exhausted` (gave up) |

### Pool metrics

| Metric | Type | Labels | Description |
|---|---|---|---|
| `halcyon_pool_connections` | Gauge | `state` | `state=idle` / `state=active` connection counts |
| `halcyon_pool_acquire_wait_seconds` | Histogram | — | Time between `pool.acquire()` call and connection hand-off |
| `halcyon_reconnects_total` | Counter | — | Transparent reconnect events (connection replaced after failure) |

### Statement cache metrics

| Metric | Type | Labels | Description |
|---|---|---|---|
| `halcyon_stmt_cache_total` | Counter | `result` | `result=hit` (reused), `miss` (prepared fresh), `evict` (LRU eviction), `overflow` (cache disabled or over limit) |
| `halcyon_stmt_cache_size` | Gauge | — | Current number of cached prepared statements per connection (emitted per-connection on change) |

## OpenTelemetry spans

Each span carries `db.system=db2`. Additional attributes per span:

| Span name | Attributes |
|---|---|
| `halcyon.query` | `db.statement`, `error.sqlstate` on failure |
| `halcyon.execute` | `db.statement`, `db.rows_affected`, `error.sqlstate` on failure |
| `halcyon.transaction` | `error.sqlstate` on rollback/commit failure |
| `halcyon.acquire` | `error.sqlstate` on timeout |
| `halcyon.reconnect` | `error.sqlstate` on failure |

Spans are emitted only when a `Tracer` is configured. The OTel adapter pulls the
active tracer from the process-global OTel provider — configure that provider in
your application as usual before opening the database.

## Implementing a custom `MetricsSink`

Wire any metrics backend by implementing the three-method interface:

```cpp
#include <halcyon/observability/metrics.hpp>

class MyMetricsSink : public halcyon::obs::MetricsSink {
public:
    void counter(std::string_view name, double value,
                 const halcyon::obs::Labels& labels) override {
        // forward to your metrics library
    }
    void histogram(std::string_view name, double value,
                   const halcyon::obs::Labels& labels) override { /* ... */ }
    void gauge(std::string_view name, double value,
               const halcyon::obs::Labels& labels) override { /* ... */ }
};

halcyon::PoolConfig config{
    .observability = { .metrics = std::make_shared<MyMetricsSink>() },
};
```

`Labels` is `std::vector<std::pair<std::string_view, std::string_view>>`. The
key/value string_views point into static storage for the duration of the call —
copy them to your own storage if you need them past the call.

## Implementing a custom `Tracer`

```cpp
#include <halcyon/observability/tracing.hpp>

class MyTracer : public halcyon::obs::Tracer {
public:
    halcyon::obs::SpanHandle startSpan(
        std::string_view name,
        const halcyon::obs::Labels& attrs) override {
        // start a span in your tracing system and return an opaque handle
        return halcyon::obs::SpanHandle{ /* ... */ };
    }
};
```

## Disabling observability at compile time

Nothing needs to be disabled: both adapters are compiled-in only when the
corresponding CMake flag is `ON`. The zero-overhead path is the default:
`has_metrics_` and `has_tracer_` are `false`, and the instrument wrapper reduces
to a single branch and a direct call to your operation.

## Prometheus Exposer example (minimal)

```cpp
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <halcyon/observability/prometheus_adapter.hpp>

auto registry = std::make_shared<prometheus::Registry>();
auto metrics  = halcyon::obs::make_prometheus_metrics(registry);

prometheus::Exposer exposer{"0.0.0.0:9090"};
exposer.RegisterCollectable(registry);

halcyon::PoolConfig cfg;
cfg.observability.metrics = metrics;
auto db = halcyon::Database::openOrThrow(dsn, cfg);
```

Scrape `http://localhost:9090/metrics` after the first query runs.
