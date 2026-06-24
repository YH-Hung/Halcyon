# Halcyon — OpenTelemetry Parent Context Propagation — Design Spec

**Date:** 2026-06-24
**Status:** Approved design (pre-implementation)
**Parent spec:** docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md (§9 Observability)

## 1. Overview

Halcyon emits tracing spans (`halcyon.query`, `halcyon.execute`,
`halcyon.transaction`, `halcyon.acquire`, `halcyon.reconnect`) through an
optional `opentelemetry-cpp` adapter. Today those spans do **not** propagate
parent context correctly: nested Halcyon spans come out as siblings instead of
children, async spans come out as orphaned roots, and there is no way for a
caller to attach Halcyon spans to an externally-supplied parent (e.g. a remote
parent reconstructed from an incoming W3C `traceparent` header).

This feature makes Halcyon a well-behaved participant in a trace: spans nest
under their logical parent, async spans inherit the caller's active span across
the executor thread boundary, and callers can hand Halcyon an explicit parent
context to continue a distributed trace.

### Current state (the three gaps)

In `src/observability/otel_adapter.cpp`, `OtelTracer::startSpan` calls
`tracer_->StartSpan(name)` with **no `StartSpanOptions`**, and `OtelSpan` does
**not** hold a `trace::Scope`. A started span is therefore never made the
*active* span. Consequences:

1. **Nested spans don't parent to each other.** `Database::transaction()`
   (`include/halcyon/database.hpp`) starts a `halcyon.transaction` span, but the
   `halcyon.execute`/`halcyon.query` spans created inside it (via `instrument()`)
   read OpenTelemetry's *implicit active context*, which was never set to the
   transaction span. They become siblings, not children. Same for the
   `halcyon.acquire` span and for any user spans created inside a
   `transaction(fn)` callback.
2. **Async loses the caller's context.** `executeAsync`/`queryAsync` run
   `instrument()` on an `Executor` worker thread. OpenTelemetry's active context
   is thread-local, so the worker thread has none — the async span becomes an
   orphaned **root** instead of a child of the caller's active span.
3. **No explicit parent.** A caller holding a parent context (e.g. extracted
   from incoming HTTP `traceparent`/`tracestate` headers) has no Halcyon API to
   make Halcyon spans children of it.

The synchronous, top-level case already works: `StartSpan` with no options uses
the thread-local active context, so a single `db.execute()` already nests under
the app's ambient span. This feature closes the remaining three gaps.

### Goals

- Nested Halcyon spans parent correctly within a thread (transaction → query →
  acquire; user spans inside callbacks).
- Async spans (`executeAsync`/`queryAsync`) inherit the caller's active context
  across the executor thread boundary.
- A caller-facing API to attach Halcyon spans to an explicit parent context,
  including continuing a distributed trace from W3C `traceparent` headers.
- Stay above the thin CLI seam **and** keep `opentelemetry-cpp` types out of all
  public headers: the parent context is opaque above the adapter.
- Core/`instrument()` wiring is unit-testable through a mock tracer, with no live
  OpenTelemetry SDK required.
- Preserve the zero-overhead invariant: when no tracer is configured, nothing on
  the new code path runs.

### Non-goals

- Outbound context injection (Halcyon does not itself make outbound calls that
  carry a `traceparent`); only inbound/extract is in scope.
- Baggage manipulation beyond what `context::Context` carries transparently.
- Propagation formats other than W3C trace-context for the `extract` helper
  (B3/Jaeger are available in opentelemetry-cpp but out of scope here).
- Metrics exemplars or any metrics-side context linkage.
- Changing span names, attributes, or the metrics surface (spec §9 names stay
  as-is).

## 2. Decisions (locked)

| Topic | Decision |
|---|---|
| Seam mechanism | Extend the `Tracer` interface with an opaque `SpanContext` + three **defaulted** methods. No OTel types in public headers. |
| Nesting fix | `OtelSpan` self-activates by holding a `trace::Scope` for its lifetime. |
| Async fix | Capture context on the calling thread at submit; pass it as an explicit parent to `startSpan` on the worker. |
| Explicit-parent entry point | RAII guard `Database::useParentContext(ctx)` — no changes to `execute`/`query`/… signatures. |
| Parent source helpers | `make_otel_active_context()` and `extract_otel_context(headers)` in the OTel adapter (OTel-free signatures). |
| Extract format | W3C trace-context (`HttpTraceContext`) over a case-insensitive header map. |
| Backward compatibility | New `Tracer` methods are non-pure with safe defaults; `NoopTracer` and existing mocks compile unchanged. |
| Zero overhead | `has_tracer_ == false` ⇒ `captureContext` never called; `instrument()` keeps its single-branch no-op fast path. |

## 3. Seam additions (`include/halcyon/observability/tracing.hpp`)

The seam stays free of any `opentelemetry-cpp` include. Two opaque,
type-erased carriers and three new `Tracer` methods:

```cpp
// Opaque, type-erased snapshot of a tracer backend's context (e.g. an OTel
// context::Context carrying the active span / remote parent). Core treats it as
// opaque; adapters subclass it. Obtained from captureContext() or an adapter
// helper (make_otel_active_context / extract_otel_context); handed back to
// startSpan(parent) or attachContext().
struct SpanContext {
    virtual ~SpanContext() = default;
};

// Opaque RAII token: while alive, a SpanContext is the active context on the
// current thread. Destruction restores the previous context. Adapters subclass
// it; core only holds and drops it.
struct ContextToken {
    virtual ~ContextToken() = default;
};

struct Tracer {
    virtual ~Tracer() = default;
    virtual std::unique_ptr<Span> startSpan(std::string_view name,
                                            const SpanAttrs& attrs) = 0;

    // Snapshot the active context on the calling thread, for handing to another
    // thread. Default: nothing to propagate.
    virtual std::shared_ptr<SpanContext> captureContext() { return nullptr; }

    // Start a span explicitly parented to `parent` (null → the active context,
    // identical to the 2-arg overload). Default: ignore parent, delegate.
    virtual std::unique_ptr<Span> startSpan(std::string_view name,
                                            const SpanAttrs& attrs,
                                            const SpanContext* parent) {
        (void)parent;
        return startSpan(name, attrs);
    }

    // Make `ctx` the active context on this thread until the returned token is
    // dropped. Default: no-op (null token). Null ctx ⇒ null token.
    virtual std::unique_ptr<ContextToken> attachContext(
        const std::shared_ptr<SpanContext>& ctx) {
        (void)ctx;
        return nullptr;
    }
};
```

`ScopedContext` — a null-tolerant, movable RAII wrapper around
`unique_ptr<ContextToken>`, mirroring the existing `ScopedSpan`. It is the public
return type of `Database::useParentContext` so callers never touch
`ContextToken` directly:

```cpp
class ScopedContext {
public:
    ScopedContext() = default;
    explicit ScopedContext(std::unique_ptr<ContextToken> t) : tok_(std::move(t)) {}
    ScopedContext(ScopedContext&&) = default;
    ScopedContext& operator=(ScopedContext&&) = default;
    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;
    explicit operator bool() const noexcept { return static_cast<bool>(tok_); }
private:
    std::unique_ptr<ContextToken> tok_;  // drops (restores prior context) on scope exit
};
```

`NoopTracer` needs no changes: the defaults give `captureContext() == nullptr`,
3-arg `startSpan` delegates to the no-op 2-arg, and `attachContext() == nullptr`.

## 4. OTel adapter (`src/observability/otel_adapter.{hpp,cpp}`)

All OpenTelemetry types stay inside the `.cpp`. Internals:

- **`OtelSpanContext : obs::SpanContext`** wraps an
  `opentelemetry::context::Context` by value.
- **`OtelContextToken : obs::ContextToken`** wraps the
  `nostd::unique_ptr<context::Token>` returned by `RuntimeContext::Attach`;
  its destructor lets the token detach (restoring the prior context).
- **`OtelSpan` self-activates.** It additionally holds a `trace::Scope` created
  via `trace::Tracer::WithActiveSpan(span_)` so the span is the active span for
  its lifetime. Declared after `span_` so it is constructed from a live handle;
  destroyed before `span_`. End ordering is LIFO because `ScopedSpan` calls
  `end()` then destroys the span, and Halcyon always starts and ends a span on
  the same thread (sync path: inside one `instrument()` call; async path: the
  whole `instrument()` runs on the worker), so the thread-local scope stack stays
  consistent.
- **`OtelTracer`** implements:
  - `captureContext()` → `make_shared<OtelSpanContext>(RuntimeContext::GetCurrent())`.
  - 3-arg `startSpan` → builds `trace::StartSpanOptions`; if `parent`
    `dynamic_cast`s to `OtelSpanContext`, sets `opts.parent = ctx` (a
    `context::Context`, from which the SDK extracts the parent span), then
    `tracer_->StartSpan(name, opts)`; applies attributes as today; wraps in the
    self-activating `OtelSpan`. The 2-arg `startSpan` forwards with `parent ==
    nullptr`.
  - `attachContext(ctx)` → if `ctx` `dynamic_cast`s to `OtelSpanContext`, returns
    `make_unique<OtelContextToken>(RuntimeContext::Attach(ctx->get()))`; else
    null.

Public helper factories (declared in `otel_adapter.hpp`, OTel-free signatures):

```cpp
// Snapshot the process's currently-active OTel context (e.g. an app span the
// caller has Scope-attached) into an opaque parent handle.
std::shared_ptr<obs::SpanContext> make_otel_active_context();

// Build an opaque parent handle by extracting W3C trace-context
// (traceparent / tracestate) from HTTP-style headers — for continuing a
// distributed trace in a server handler. Header keys are matched
// case-insensitively per the W3C spec.
std::shared_ptr<obs::SpanContext> extract_otel_context(
    const std::map<std::string, std::string>& headers);
```

`extract_otel_context` implements a `TextMapCarrier` over the header map and runs
`opentelemetry::trace::propagation::HttpTraceContext().Extract(carrier,
RuntimeContext::GetCurrent())`, returning the resulting context wrapped in an
`OtelSpanContext`.

## 5. Core wiring (`include/halcyon/database.hpp`, `pool.hpp`)

- **`instrument()`** gains a trailing parameter `const obs::SpanContext* parent =
  nullptr`. When non-null and a tracer is present, it starts the span via the
  3-arg `startSpan(spanName, attrs, parent)`; otherwise it uses the existing
  2-arg call. The `!hasM && !hasT` zero-overhead early return is unchanged.
- **`executeAsync` / `queryAsync`:** on the **calling thread**, before
  `exec_->submit(...)`, capture
  `auto parent = has_tracer_ ? tracer_->captureContext() : nullptr;`. Move the
  `shared_ptr<SpanContext>` into the task lambda and pass `parent.get()` to
  `instrument()`. This re-parents the async span under the caller's active
  context (which includes any `useParentContext` guard the caller holds). When
  `has_tracer_` is false, no capture happens.
- **Synchronous paths** (`execute`/`query`/`queryAs`/`transaction`, and the pool
  `acquire`/`reconnect` spans) need **no** parent argument: with `OtelSpan`
  self-activation, nested spans pick up the correct parent through the implicit
  active context, and a caller's `useParentContext` guard is the active context
  for those calls too.
- **New public method on `Database`:**

```cpp
// Parent all Halcyon spans created on the current thread under `ctx` until the
// returned guard is destroyed — synchronous calls and any async work submitted
// while it is held. `ctx` comes from the OTel adapter (make_otel_active_context
// / extract_otel_context). No-op when no tracer is configured.
[[nodiscard]] obs::ScopedContext useParentContext(std::shared_ptr<obs::SpanContext> ctx) {
    return obs::ScopedContext(has_tracer_ ? tracer_->attachContext(std::move(ctx))
                                          : nullptr);
}
```

### Resulting parent/child structure

- `transaction()` → `halcyon.transaction` is active; inner `execute`/`query`
  spans and their `halcyon.acquire` spans nest under it.
- A bare `query()` → `halcyon.query` active; its `halcyon.acquire` nests under it.
- `queryAsync()` under a caller span → the worker's `halcyon.query` is a child of
  the captured caller context.
- Inside `db.useParentContext(extract_otel_context(headers))` scope → all of the
  above hang off the remote parent.

## 6. Testing

### Unit (`tests/unit/test_observability.cpp`, no DB, no OTel SDK)

Extend the existing `RecordingTracer`/`RecordingSpan` doubles to model context:
- A thread-local stack of "active context" handles; `attachContext` pushes and
  returns a `RecordingContextToken` that pops on destruction; `captureContext`
  returns the current top (or a fresh root marker); each `SpanRecord` records the
  `parent` identity it was started under (top-of-stack for 2-arg, the passed
  `parent` for 3-arg).
- New `RecordingSpan` self-activation: starting a span pushes it as active and
  ending pops it, mirroring `OtelSpan`'s scope, so nesting is observable.

New tests:
- **Nesting:** `transaction()` with an inner `execute` ⇒ the `halcyon.execute`
  span's parent is the `halcyon.transaction` span; the `halcyon.acquire` span's
  parent is the surrounding query/execute span.
- **Async inheritance:** under `db.useParentContext(ctx)`, a `queryAsync(...)`
  span's parent is `ctx` even though it ran on an executor thread.
- **Explicit parent:** `useParentContext(ctx)` then a sync `execute` ⇒ span
  parent is `ctx`; guard destruction restores the prior active context.
- **Zero overhead unchanged:** with no tracer, `captureContext` is never invoked
  and the existing no-tracer assertions still hold.

### Integration (`HALCYON_WITH_OTEL=ON`, CTest label `integration`, gated)

Use an in-memory span exporter (opentelemetry-cpp `InMemorySpanExporter` +
`SimpleSpanProcessor`) and assert real `SpanId`/`TraceId` links:
- nested transaction→query→acquire share a trace and chain parent IDs;
- `queryAsync` under an app span is a child of that span;
- `extract_otel_context(headers)` with a synthetic `traceparent` yields spans
  whose `TraceId` matches the header and whose parent is the remote span ID.

### Tooling

`-Wall -Wextra -Wpedantic` clean; `clang-format`/`clang-tidy` before commit. Run
the full suite per AGENTS.md, including live-Db2 integration tests, before
claiming completion.

## 7. Documentation

- Update parent spec §9 "Tracing spans" to note parent-context propagation
  (nesting, async inheritance, and the `useParentContext` /
  `extract_otel_context` entry points).
- Add a short usage snippet to the observability example/README showing a server
  handler continuing a distributed trace.
- AGENTS.md needs no change (architecture and seam invariants are unchanged).

## 8. Risks & mitigations

- **Scope/thread discipline.** `trace::Scope` mutates thread-local runtime
  context; a span started on one thread and ended on another would corrupt the
  stack. Mitigation: Halcyon already starts and ends every span on a single
  thread (sync within one `instrument()`; async entirely on the worker). The
  design adds no cross-thread span object; only the opaque `SpanContext`
  snapshot crosses threads, and it is immutable.
- **opentelemetry-cpp API drift.** Exact helpers (`WithActiveSpan` vs `Scope`
  ctor vs `RuntimeContext::Attach`; `StartSpanOptions::parent` variant) are
  verified against the vendored version (`/opt/homebrew/include/opentelemetry`
  confirms `StartSpanOptions::parent` accepts `context::Context`,
  `Tracer::WithActiveSpan`, and `trace/propagation/http_trace_context.h`).
- **Interface growth on the seam.** Three new `Tracer` methods. Mitigated by
  defaulting all three so no existing implementer breaks, and by keeping them
  opaque (no OTel types leak).

## 9. Open items

None blocking.
