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

struct Tracer {
    virtual ~Tracer() = default;
    virtual std::unique_ptr<Span> startSpan(std::string_view name,
                                            const SpanAttrs& attrs) = 0;
};

struct NoopSpan final : Span {
    void setAttribute(std::string_view, std::string_view) override {}
    void setStatusError(std::string_view) override {}
    void end() override {}
};

struct NoopTracer final : Tracer {
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

}  // namespace halcyon::obs
