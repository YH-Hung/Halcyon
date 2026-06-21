#include "halcyon/observability/otel_adapter.hpp"

#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/string_view.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/tracer.h>

#include <string>
#include <utility>

namespace halcyon::obs {
namespace {

namespace otel = opentelemetry;
namespace trace = opentelemetry::trace;

otel::nostd::string_view sv(std::string_view s) {
    return otel::nostd::string_view(s.data(), s.size());
}

class OtelSpan final : public Span {
public:
    explicit OtelSpan(otel::nostd::shared_ptr<trace::Span> span)
        : span_(std::move(span)) {}
    void setAttribute(std::string_view key, std::string_view value) override {
        span_->SetAttribute(sv(key), sv(value));
    }
    void setStatusError(std::string_view sqlstate) override {
        span_->SetStatus(trace::StatusCode::kError, std::string(sqlstate));
    }
    void end() override { span_->End(); }

private:
    otel::nostd::shared_ptr<trace::Span> span_;
};

class OtelTracer final : public Tracer {
public:
    explicit OtelTracer(otel::nostd::shared_ptr<trace::Tracer> tracer)
        : tracer_(std::move(tracer)) {}
    std::unique_ptr<Span> startSpan(std::string_view name,
                                    const SpanAttrs& attrs) override {
        auto span = tracer_->StartSpan(sv(name));
        for (const auto& [k, v] : attrs) span->SetAttribute(sv(k), sv(v));
        return std::make_unique<OtelSpan>(std::move(span));
    }

private:
    otel::nostd::shared_ptr<trace::Tracer> tracer_;
};

}  // namespace

std::shared_ptr<Tracer> make_otel_tracer(std::string_view tracer_name) {
    auto provider = trace::Provider::GetTracerProvider();
    auto tracer = provider->GetTracer(sv(tracer_name));
    return std::make_shared<OtelTracer>(std::move(tracer));
}

}  // namespace halcyon::obs
