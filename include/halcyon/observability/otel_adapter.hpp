#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "halcyon/observability/tracing.hpp"

namespace halcyon::obs {

// Builds a Tracer that emits spans through the process-global OpenTelemetry
// tracer provider (configure that provider in your application as usual).
std::shared_ptr<Tracer> make_otel_tracer(std::string_view tracer_name = "halcyon");

// Snapshot the process's currently-active OpenTelemetry context (e.g. an
// application span the caller has made active) into an opaque parent handle.
std::shared_ptr<SpanContext> make_otel_active_context();

// Build an opaque parent handle by extracting W3C trace-context
// (traceparent / tracestate) from HTTP-style headers, to continue a distributed
// trace in a server handler. Header keys are matched case-insensitively.
std::shared_ptr<SpanContext> extract_otel_context(
    const std::map<std::string, std::string>& headers);

}  // namespace halcyon::obs
