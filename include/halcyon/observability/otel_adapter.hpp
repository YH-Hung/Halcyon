#pragma once

#include <memory>
#include <string_view>

#include "halcyon/observability/tracing.hpp"

// Factory for an opentelemetry-cpp-backed Tracer. The declaration exposes no
// OpenTelemetry types (the adapter pulls the active tracer from the global
// provider internally), so this header is dependency-free. The definition is
// compiled in only when HALCYON_WITH_OTEL is ON and opentelemetry-cpp was found.
namespace halcyon::obs {

// Builds a Tracer that emits spans through the process-global OpenTelemetry
// tracer provider (configure that provider in your application as usual).
std::shared_ptr<Tracer> make_otel_tracer(std::string_view tracer_name = "halcyon");

}  // namespace halcyon::obs
