#pragma once

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace halcyon::obs {

// Initial attributes for a span. As with Labels, the views must outlive the
// startSpan call; adapters copy if they retain them.
using SpanAttrs = std::vector<std::pair<std::string_view, std::string_view>>;

// A single tracing span (spec §9). end() is idempotent; ScopedSpan calls it.
struct Span {
    virtual ~Span() = default;
    virtual void setAttribute(std::string_view key, std::string_view value) = 0;
    virtual void setStatusError(std::string_view sqlstate) = 0;
    virtual void end() = 0;
};

// Opaque, type-erased snapshot of a tracer backend's parent context (e.g. an
// OpenTelemetry context::Context carrying the active span or a remote parent).
// Core treats it as opaque; adapters subclass it. Obtained from
// Tracer::captureContext() or an adapter helper (make_otel_active_context /
// extract_otel_context) and handed back to startSpan()/attachContext().
struct SpanContext {
    virtual ~SpanContext() = default;
};

// Opaque RAII token: while alive, a SpanContext is the active context on the
// current thread; destruction restores the previously-active context. Adapters
// subclass it; core only holds and drops it via ScopedContext.
struct ContextToken {
    virtual ~ContextToken() = default;
};

struct Tracer {
    virtual ~Tracer() = default;
    virtual std::unique_ptr<Span> startSpan(std::string_view name,
                                            const SpanAttrs& attrs) = 0;

    // Snapshot the active context on the calling thread, for handing to another
    // thread (e.g. an async worker). Default: nothing to propagate.
    virtual std::shared_ptr<SpanContext> captureContext() { return nullptr; }

    // Start a span explicitly parented to `parent`. A null parent means "use the
    // active context", identical to the two-argument overload. Default: ignore
    // the parent and delegate, so existing tracers need no changes.
    virtual std::unique_ptr<Span> startSpan(std::string_view name,
                                            const SpanAttrs& attrs,
                                            const SpanContext* parent) {
        (void)parent;
        return startSpan(name, attrs);
    }

    // Make `ctx` the active context on the current thread until the returned
    // token is dropped. Default: no-op (null token). A null ctx yields a null
    // token.
    virtual std::unique_ptr<ContextToken> attachContext(
        const std::shared_ptr<SpanContext>& ctx) {
        (void)ctx;
        return nullptr;
    }
};

struct NoopSpan final : Span {
    void setAttribute(std::string_view, std::string_view) override {}
    void setStatusError(std::string_view) override {}
    void end() override {}
};

struct NoopTracer final : Tracer {
    using Tracer::startSpan;  // bring all overloads into scope
    std::unique_ptr<Span> startSpan(std::string_view,
                                    const SpanAttrs&) override {
        return std::make_unique<NoopSpan>();
    }
};

inline Tracer& noop_tracer() {
    static NoopTracer t;
    return t;
}

// RAII wrapper that ends the span on scope exit and tolerates an empty (null)
// span, so instrumented call sites can write one straight-line code path that
// costs nothing when tracing is disabled.
class ScopedSpan {
public:
    ScopedSpan() = default;
    explicit ScopedSpan(std::unique_ptr<Span> span) : span_(std::move(span)) {}
    ScopedSpan(ScopedSpan&&) = default;
    ScopedSpan& operator=(ScopedSpan&&) = default;
    ScopedSpan(const ScopedSpan&) = delete;
    ScopedSpan& operator=(const ScopedSpan&) = delete;
    ~ScopedSpan() {
        if (span_) span_->end();
    }

    explicit operator bool() const noexcept { return static_cast<bool>(span_); }
    void setAttribute(std::string_view k, std::string_view v) {
        if (span_) span_->setAttribute(k, v);
    }
    void setStatusError(std::string_view sqlstate) {
        if (span_) span_->setStatusError(sqlstate);
    }

private:
    std::unique_ptr<Span> span_;
};

// RAII wrapper that drops a ContextToken (restoring the previously-active
// context) on scope exit and tolerates an empty (null) token, mirroring
// ScopedSpan so call sites cost nothing when tracing is disabled.
class ScopedContext {
public:
    ScopedContext() = default;
    explicit ScopedContext(std::unique_ptr<ContextToken> tok)
        : tok_(std::move(tok)) {}
    ScopedContext(ScopedContext&&) = default;
    ScopedContext& operator=(ScopedContext&&) = default;
    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;
    explicit operator bool() const noexcept { return static_cast<bool>(tok_); }

private:
    std::unique_ptr<ContextToken> tok_;
};

}  // namespace halcyon::obs
