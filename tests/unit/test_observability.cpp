#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "halcyon/database.hpp"
#include "halcyon/observability/metrics.hpp"
#include "halcyon/observability/tracing.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;
namespace obs = halcyon::obs;

// File scope (not anonymous) and not named "Row" — halcyon::Row already exists.
struct ObsRow {
    std::int64_t n;
};
HALCYON_REFLECT(ObsRow, n);

namespace {

struct MetricSample {
    std::string name;
    double value;
    std::map<std::string, std::string> labels;
};

// Records every metric emission for later assertion. Thread-safe so async paths
// (worker threads) can be exercised too.
class RecordingMetricsSink : public obs::MetricsSink {
public:
    void counter(std::string_view n, double v, const obs::Labels& l) override {
        rec(n, v, l);
    }
    void histogram(std::string_view n, double v, const obs::Labels& l) override {
        rec(n, v, l);
    }
    void gauge(std::string_view n, double v, const obs::Labels& l) override {
        rec(n, v, l);
    }

    std::vector<MetricSample> samples;
    mutable std::mutex mu;

private:
    void rec(std::string_view n, double v, const obs::Labels& l) {
        std::lock_guard<std::mutex> lk(mu);
        MetricSample s;
        s.name = std::string(n);
        s.value = v;
        for (const auto& [k, val] : l) s.labels[std::string(k)] = std::string(val);
        samples.push_back(std::move(s));
    }
};

bool labelsMatch(const std::map<std::string, std::string>& have,
                 const std::map<std::string, std::string>& want) {
    for (const auto& [k, v] : want) {
        auto it = have.find(k);
        if (it == have.end() || it->second != v) return false;
    }
    return true;
}

int count(const RecordingMetricsSink& s, const std::string& name,
          std::map<std::string, std::string> labels = {}) {
    std::lock_guard<std::mutex> lk(s.mu);
    int n = 0;
    for (const auto& smp : s.samples)
        if (smp.name == name && labelsMatch(smp.labels, labels)) ++n;
    return n;
}

double maxValue(const RecordingMetricsSink& s, const std::string& name,
                std::map<std::string, std::string> labels = {}) {
    std::lock_guard<std::mutex> lk(s.mu);
    double m = -1;
    for (const auto& smp : s.samples)
        if (smp.name == name && labelsMatch(smp.labels, labels))
            m = std::max(m, smp.value);
    return m;
}

struct SpanRecord {
    std::string name;
    std::map<std::string, std::string> attrs;
    bool errored = false;
};

class RecordingSpan : public obs::Span {
public:
    explicit RecordingSpan(SpanRecord* r) : r_(r) {}
    void setAttribute(std::string_view k, std::string_view v) override {
        r_->attrs[std::string(k)] = std::string(v);
    }
    void setStatusError(std::string_view) override { r_->errored = true; }
    void end() override {}

private:
    SpanRecord* r_;
};

class RecordingTracer : public obs::Tracer {
public:
    std::unique_ptr<obs::Span> startSpan(std::string_view name,
                                         const obs::SpanAttrs& attrs) override {
        std::lock_guard<std::mutex> lk(mu);
        auto rec = std::make_shared<SpanRecord>();
        rec->name = std::string(name);
        for (const auto& [k, v] : attrs)
            rec->attrs[std::string(k)] = std::string(v);
        spans.push_back(rec);
        return std::make_unique<RecordingSpan>(rec.get());
    }
    std::vector<std::shared_ptr<SpanRecord>> spans;
    mutable std::mutex mu;
};

const SpanRecord* findSpan(const RecordingTracer& t, const std::string& name) {
    std::lock_guard<std::mutex> lk(t.mu);
    for (const auto& s : t.spans)
        if (s->name == name) return s.get();
    return nullptr;
}

PoolConfig withMetrics(std::shared_ptr<obs::MetricsSink> m) {
    PoolConfig c;
    c.startMaintenanceThread = false;
    c.observability.metrics = std::move(m);
    return c;
}

}  // namespace

TEST(Observability, ExecuteEmitsQueriesTotalAndDuration) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    auto m = std::make_shared<RecordingMetricsSink>();
    auto db = Database::open(driver, "X", withMetrics(m)).value();

    ASSERT_TRUE(db.execute("INSERT INTO t VALUES (?)", 1).ok());

    EXPECT_EQ(count(*m, "halcyon_queries_total", {{"op", "insert"}, {"status", "ok"}}),
              1);
    EXPECT_EQ(count(*m, "halcyon_query_duration_seconds", {{"op", "insert"}}), 1);
    EXPECT_EQ(count(*m, "halcyon_errors_total"), 0);
}

TEST(Observability, ErrorPathEmitsErrorsTotalAndErrorStatus) {
    MockCliDriver driver;
    halcyon::Error e;
    e.code = halcyon::ErrorCode::Syntax;
    e.message = "bad sql";
    driver.executeErrors.push_back(e);  // non-retriable
    auto m = std::make_shared<RecordingMetricsSink>();
    auto db = Database::open(driver, "X", withMetrics(m)).value();

    ASSERT_FALSE(db.execute("INSERT INTO t VALUES (?)", 1).ok());

    EXPECT_EQ(count(*m, "halcyon_queries_total",
                    {{"op", "insert"}, {"status", "error"}}),
              1);
    EXPECT_EQ(count(*m, "halcyon_errors_total", {{"code", "syntax"}}), 1);
}

TEST(Observability, RetriesEmitRetriedThenExhausted) {
    MockCliDriver driver;
    for (int i = 0; i < 3; ++i) {  // every attempt fails, retriably
        halcyon::Error e;
        e.code = halcyon::ErrorCode::Deadlock;
        e.retriable = true;
        e.message = "deadlock";
        driver.executeErrors.push_back(e);
    }
    auto m = std::make_shared<RecordingMetricsSink>();
    PoolConfig cfg = withMetrics(m);
    cfg.backoff.maxAttempts = 3;
    cfg.backoff.sleep = [](std::chrono::milliseconds) {};  // instant
    auto db = Database::open(driver, "X", cfg).value();

    // SELECT is read-only → auto-retried up to maxAttempts.
    auto r = db.queryAs<ObsRow>("SELECT n FROM t");
    ASSERT_FALSE(r.ok());

    EXPECT_EQ(count(*m, "halcyon_retries_total", {{"outcome", "retried"}}), 2);
    EXPECT_EQ(count(*m, "halcyon_retries_total", {{"outcome", "exhausted"}}), 1);
    EXPECT_EQ(count(*m, "halcyon_queries_total", {{"status", "error"}}), 1);
}

TEST(Observability, PoolGaugeAndAcquireWaitEmitted) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(0);
    auto m = std::make_shared<RecordingMetricsSink>();
    PoolConfig cfg = withMetrics(m);
    cfg.min = 2;
    cfg.max = 4;
    auto db = Database::open(driver, "X", cfg).value();  // warms to min=2

    // Warmup set the idle gauge up to 2.
    EXPECT_EQ(maxValue(*m, "halcyon_pool_connections", {{"state", "idle"}}), 2.0);

    ASSERT_TRUE(db.execute("UPDATE t SET a=?", 1).ok());  // one acquire
    EXPECT_GE(count(*m, "halcyon_pool_acquire_wait_seconds"), 1);
}

TEST(Observability, SpansCarrySystemAndStatement) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);  // the INSERT's affected-row count
    auto t = std::make_shared<RecordingTracer>();
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    cfg.observability.tracer = t;
    auto db = Database::open(driver, "X", cfg).value();

    ASSERT_TRUE(db.execute("INSERT INTO t VALUES (?)", 1).ok());
    ASSERT_TRUE(db.query("SELECT n FROM t").ok());

    const SpanRecord* exec = findSpan(*t, "halcyon.execute");
    ASSERT_NE(exec, nullptr);
    EXPECT_EQ(exec->attrs.at("db.system"), "db2");
    EXPECT_EQ(exec->attrs.at("db.statement"), "INSERT INTO t VALUES (?)");
    EXPECT_EQ(exec->attrs.at("db.rows_affected"), "1");  // spec §9 attr
    const SpanRecord* q = findSpan(*t, "halcyon.query");
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->attrs.at("db.system"), "db2");
    EXPECT_NE(findSpan(*t, "halcyon.acquire"), nullptr);  // pool span too
}

TEST(Observability, ErrorPathMarksSpanErrored) {
    MockCliDriver driver;
    halcyon::Error e;
    e.code = halcyon::ErrorCode::Syntax;
    e.message = "bad sql";
    driver.executeErrors.push_back(e);
    auto t = std::make_shared<RecordingTracer>();
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    cfg.observability.tracer = t;
    auto db = Database::open(driver, "X", cfg).value();

    ASSERT_FALSE(db.execute("INSERT INTO t VALUES (?)", 1).ok());

    const SpanRecord* s = findSpan(*t, "halcyon.execute");
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->errored);  // failure must mark the span's status as error
}

TEST(Observability, ReconnectEmitsReconnectsTotal) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(0);
    driver.aliveResults.push_back(false);  // pooled conn deemed dead on acquire
    auto m = std::make_shared<RecordingMetricsSink>();
    PoolConfig cfg = withMetrics(m);
    cfg.min = 1;
    cfg.validateOnAcquire = true;  // forces a validate -> transparent reconnect
    auto db = Database::open(driver, "X", cfg).value();

    ASSERT_TRUE(db.execute("UPDATE t SET a=?", 1).ok());

    EXPECT_GE(count(*m, "halcyon_reconnects_total"), 1);
}

// A metrics sink that re-enters the pool on every emission. If the pool emitted
// while holding its own mutex, the re-entrant idle_count()/active_count() (which
// lock the same mutex) would deadlock. Reaching the end proves emission happens
// after the lock is released.
class ReentrantSink : public RecordingMetricsSink {
public:
    halcyon::ConnectionPool* pool = nullptr;
    void gauge(std::string_view n, double v, const obs::Labels& l) override {
        if (pool) {
            (void)pool->idle_count();
            (void)pool->active_count();
        }
        RecordingMetricsSink::gauge(n, v, l);
    }
};

TEST(Observability, SinkMayReenterPoolWithoutDeadlock) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(0);
    auto m = std::make_shared<ReentrantSink>();
    PoolConfig cfg = withMetrics(m);
    cfg.min = 1;
    auto db = Database::open(driver, "X", cfg).value();
    m->pool = &db.pool();  // re-entry armed for acquire/release emissions

    ASSERT_TRUE(db.execute("UPDATE t SET a=?", 1).ok());  // acquire + release

    SUCCEED();  // no deadlock
}

TEST(Observability, MaintainGaugeNeverNegativeDuringReap) {
    MockCliDriver driver;
    auto m = std::make_shared<RecordingMetricsSink>();
    PoolConfig cfg = withMetrics(m);
    cfg.min = 1;
    cfg.max = 4;
    cfg.idleTimeout = std::chrono::milliseconds(0);  // idle slots reapable
    cfg.maxLifetime = std::chrono::hours(2);         // not lifetime-expired
    auto clock = std::make_shared<halcyon::Clock::time_point>(
        halcyon::Clock::now());
    cfg.now = [clock] { return *clock; };
    auto db = Database::open(driver, "X", cfg).value();

    // Force growth to 3 active, then release all -> 3 idle (over min=1).
    {
        std::vector<halcyon::PooledConnection> leases;
        for (int i = 0; i < 3; ++i) leases.push_back(db.pool().acquire().value());
    }
    *clock += std::chrono::hours(1);  // age the idle slots past idleTimeout
    db.pool().maintain();             // reaps 2; emits one gauge at final state

    // The reap must never publish a negative active count.
    std::lock_guard<std::mutex> lk(m->mu);
    for (const auto& s : m->samples)
        if (s.name == "halcyon_pool_connections" &&
            s.labels.count("state") && s.labels.at("state") == "active")
            EXPECT_GE(s.value, 0.0);
}

TEST(Observability, DefaultConfigResolvesToNoopSinks) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(0);
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    auto db = Database::open(driver, "X", cfg).value();

    // Zero-overhead default: no real sinks, resolved to the process-wide no-ops.
    EXPECT_EQ(db.pool().metrics(), &obs::noop_metrics_sink());
    EXPECT_EQ(db.pool().tracer(), &obs::noop_tracer());
    EXPECT_FALSE(db.pool().metrics_enabled());
    EXPECT_FALSE(db.pool().tracer_enabled());
    EXPECT_TRUE(db.execute("UPDATE t SET a=?", 1).ok());  // still works
}
