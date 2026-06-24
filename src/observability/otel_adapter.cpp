#include "halcyon/observability/otel_adapter.hpp"

#include <opentelemetry/context/context.h>
#include <opentelemetry/context/propagation/text_map_propagator.h>
#include <opentelemetry/context/runtime_context.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/string_view.h>
#include <opentelemetry/nostd/unique_ptr.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/propagation/http_trace_context.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/span_startoptions.h>
#include <opentelemetry/trace/tracer.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <utility>

namespace halcyon::obs {
namespace {

namespace otel = opentelemetry;
namespace trace = opentelemetry::trace;
namespace context = opentelemetry::context;

otel::nostd::string_view sv(std::string_view s) {
    return otel::nostd::string_view(s.data(), s.size());
}

// Opaque parent-context carrier holding a full OTel context::Context.
class OtelSpanContext final : public SpanContext {
public:
    explicit OtelSpanContext(context::Context ctx) : ctx_(std::move(ctx)) {}
    const context::Context& get() const { return ctx_; }

private:
    context::Context ctx_;
};

// Opaque RAII token: keeps a context active on the current thread until dropped.
class OtelContextToken final : public ContextToken {
public:
    explicit OtelContextToken(otel::nostd::unique_ptr<context::Token> tok)
        : tok_(std::move(tok)) {}

private:
    otel::nostd::unique_ptr<context::Token> tok_;  // detaches on destruction
};

class OtelSpan final : public Span {
public:
    explicit OtelSpan(otel::nostd::shared_ptr<trace::Span> span)
        : span_(std::move(span)) {
        // Self-activate: make this span the active span for its lifetime so
        // nested Halcyon/user spans parent to it. Detaches when token_ is
        // destroyed (after end()).
        auto cur = context::RuntimeContext::GetCurrent();
        token_ = context::RuntimeContext::Attach(trace::SetSpan(cur, span_));
    }
    void setAttribute(std::string_view key, std::string_view value) override {
        span_->SetAttribute(sv(key), sv(value));
    }
    void setStatusError(std::string_view sqlstate) override {
        span_->SetStatus(trace::StatusCode::kError, std::string(sqlstate));
    }
    // Ends the OTel span but intentionally leaves token_ attached; the active
    // context is detached only when this OtelSpan is destroyed. ScopedSpan
    // destroys it immediately after end(), on the same thread, with no
    // intervening startSpan, so detaches stay strictly LIFO. Do not introduce a
    // path that starts another span on this thread between an early end() and
    // this object's destruction.
    void end() override { span_->End(); }

private:
    otel::nostd::shared_ptr<trace::Span> span_;
    // Declared after span_ so the context Token is destroyed (detached) before
    // the span handle is released.
    otel::nostd::unique_ptr<context::Token> token_;
};

class OtelTracer final : public Tracer {
public:
    explicit OtelTracer(otel::nostd::shared_ptr<trace::Tracer> tracer)
        : tracer_(std::move(tracer)) {}

    std::unique_ptr<Span> startSpan(std::string_view name,
                                    const SpanAttrs& attrs) override {
        return startSpan(name, attrs, nullptr);
    }
    std::unique_ptr<Span> startSpan(std::string_view name, const SpanAttrs& attrs,
                                    const SpanContext* parent) override {
        trace::StartSpanOptions opts;
        if (const auto* oc = dynamic_cast<const OtelSpanContext*>(parent))
            opts.parent = oc->get();
        auto span = tracer_->StartSpan(sv(name), opts);
        for (const auto& [k, v] : attrs) span->SetAttribute(sv(k), sv(v));
        return std::make_unique<OtelSpan>(std::move(span));
    }
    std::shared_ptr<SpanContext> captureContext() override {
        return std::make_shared<OtelSpanContext>(
            context::RuntimeContext::GetCurrent());
    }
    std::unique_ptr<ContextToken> attachContext(
        const std::shared_ptr<SpanContext>& ctx) override {
        const auto* oc = dynamic_cast<const OtelSpanContext*>(ctx.get());
        if (!oc) return nullptr;
        return std::make_unique<OtelContextToken>(
            context::RuntimeContext::Attach(oc->get()));
    }

private:
    otel::nostd::shared_ptr<trace::Tracer> tracer_;
};

// Case-insensitive read-only header carrier for W3C trace-context extraction.
class HeaderCarrier final : public context::propagation::TextMapCarrier {
public:
    explicit HeaderCarrier(const std::map<std::string, std::string>& h)
        : headers_(h) {}
    otel::nostd::string_view Get(
        otel::nostd::string_view key) const noexcept override {
        std::string want(key.data(), key.size());
        std::transform(want.begin(), want.end(), want.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        for (const auto& [hk, hv] : headers_) {
            std::string have = hk;
            std::transform(have.begin(), have.end(), have.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (have == want) return sv(hv);
        }
        return "";
    }
    void Set(otel::nostd::string_view, otel::nostd::string_view) noexcept override {
    }

private:
    const std::map<std::string, std::string>& headers_;
};

}  // namespace

std::shared_ptr<Tracer> make_otel_tracer(std::string_view tracer_name) {
    auto provider = trace::Provider::GetTracerProvider();
    auto tracer = provider->GetTracer(sv(tracer_name));
    return std::make_shared<OtelTracer>(std::move(tracer));
}

std::shared_ptr<SpanContext> make_otel_active_context() {
    return std::make_shared<OtelSpanContext>(
        context::RuntimeContext::GetCurrent());
}

std::shared_ptr<SpanContext> extract_otel_context(
    const std::map<std::string, std::string>& headers) {
    HeaderCarrier carrier(headers);
    trace::propagation::HttpTraceContext propagator;
    auto current = context::RuntimeContext::GetCurrent();
    return std::make_shared<OtelSpanContext>(propagator.Extract(carrier, current));
}

}  // namespace halcyon::obs
