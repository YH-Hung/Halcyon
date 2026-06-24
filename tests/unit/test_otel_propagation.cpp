#include <gtest/gtest.h>

#include <opentelemetry/exporters/memory/in_memory_span_data.h>
#include <opentelemetry/exporters/memory/in_memory_span_exporter.h>
#include <opentelemetry/sdk/trace/simple_processor.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/scope.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "halcyon/database.hpp"
#include "halcyon/observability/otel_adapter.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;

namespace otrace = opentelemetry::trace;
namespace sdktrace = opentelemetry::sdk::trace;
namespace memexp = opentelemetry::exporter::memory;

struct OtelN {
    std::int64_t n;
};
HALCYON_REFLECT(OtelN, n);

namespace {

// Installs an in-memory-backed SDK TracerProvider as the global provider and
// exposes the collected span data.
struct OtelHarness {
    std::shared_ptr<memexp::InMemorySpanData> data;
    OtelHarness() {
        auto exporter = std::unique_ptr<memexp::InMemorySpanExporter>(
            new memexp::InMemorySpanExporter());
        data = exporter->GetData();
        auto processor = std::unique_ptr<sdktrace::SpanProcessor>(
            new sdktrace::SimpleSpanProcessor(std::move(exporter)));
        std::shared_ptr<otrace::TracerProvider> provider(
            new sdktrace::TracerProvider(std::move(processor)));
        otrace::Provider::SetTracerProvider(provider);
    }
};

const sdktrace::SpanData* findExported(
    const std::vector<std::unique_ptr<sdktrace::SpanData>>& spans,
    const char* name) {
    for (const auto& s : spans)
        if (s->GetName() == name) return s.get();
    return nullptr;
}

PoolConfig otelCfg() {
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    cfg.observability.tracer = halcyon::obs::make_otel_tracer();
    return cfg;
}

}  // namespace

TEST(OtelAdapter, NestedSpansShareTraceAndChain) {
    OtelHarness h;
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    auto db = Database::open(driver, "X", otelCfg()).value();

    ASSERT_TRUE(db.execute("INSERT INTO t VALUES (?)", 1).ok());

    auto spans = h.data->GetSpans();
    const auto* exec = findExported(spans, "halcyon.execute");
    const auto* acq = findExported(spans, "halcyon.acquire");
    ASSERT_NE(exec, nullptr);
    ASSERT_NE(acq, nullptr);
    EXPECT_EQ(std::memcmp(acq->GetTraceId().Id().data(),
                          exec->GetTraceId().Id().data(), 16),
              0);
    EXPECT_EQ(std::memcmp(acq->GetParentSpanId().Id().data(),
                          exec->GetSpanId().Id().data(), 8),
              0);
}

TEST(OtelAdapter, ExtractContinuesRemoteTrace) {
    OtelHarness h;
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    auto db = Database::open(driver, "X", otelCfg()).value();

    std::map<std::string, std::string> headers{
        {"traceparent",
         "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01"}};
    {
        auto guard =
            db.useParentContext(halcyon::obs::extract_otel_context(headers));
        ASSERT_TRUE(db.execute("INSERT INTO t VALUES (?)", 1).ok());
    }

    auto spans = h.data->GetSpans();
    const auto* exec = findExported(spans, "halcyon.execute");
    ASSERT_NE(exec, nullptr);
    const std::uint8_t want_trace[16] = {0x0a, 0xf7, 0x65, 0x19, 0x16, 0xcd,
                                         0x43, 0xdd, 0x84, 0x48, 0xeb, 0x21,
                                         0x1c, 0x80, 0x31, 0x9c};
    const std::uint8_t want_parent[8] = {0xb7, 0xad, 0x6b, 0x71,
                                         0x69, 0x20, 0x33, 0x31};
    EXPECT_EQ(std::memcmp(exec->GetTraceId().Id().data(), want_trace, 16), 0);
    EXPECT_EQ(std::memcmp(exec->GetParentSpanId().Id().data(), want_parent, 8),
              0);
}

TEST(OtelAdapter, AsyncInheritsActiveSpanAcrossThreads) {
    OtelHarness h;
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"n"}, {{halcyon::detail::cli::Value{std::int64_t{3}}}}});
    auto db = Database::open(driver, "X", otelCfg()).value();

    auto tracer = otrace::Provider::GetTracerProvider()->GetTracer("test");
    auto app = tracer->StartSpan("app");
    std::future<halcyon::Result<std::vector<OtelN>>> fut;
    {
        otrace::Scope scope(app);  // app active on this thread
        fut = db.queryAsync<OtelN>("SELECT n FROM t");
    }
    auto r = fut.get();
    ASSERT_TRUE(r.ok());
    app->End();

    auto spans = h.data->GetSpans();
    const auto* q = findExported(spans, "halcyon.query");
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(std::memcmp(q->GetTraceId().Id().data(),
                          app->GetContext().trace_id().Id().data(), 16),
              0);
    EXPECT_EQ(std::memcmp(q->GetParentSpanId().Id().data(),
                          app->GetContext().span_id().Id().data(), 8),
              0);
}
