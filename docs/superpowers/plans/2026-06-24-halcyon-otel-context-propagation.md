# OpenTelemetry Parent Context Propagation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Halcyon a well-behaved trace participant — nested spans parent correctly, async spans inherit the caller's active context across the executor thread boundary, and callers can attach Halcyon spans to an explicit parent (e.g. a remote parent from a W3C `traceparent` header).

**Architecture:** Add an opaque, type-erased `SpanContext`/`ContextToken` plus three *defaulted* methods to the `Tracer` seam (no OpenTelemetry types in public headers). The OTel adapter self-activates spans (fixes nesting) and implements capture/attach/explicit-parent. `Database` captures context at async-submit time and exposes a `useParentContext` RAII guard. Core wiring is unit-tested through a recording mock; real `SpanId` links are verified by an OTel-gated test.

**Tech Stack:** C++17, CMake ≥ 3.20, GoogleTest, opentelemetry-cpp (API + SDK + in-memory span exporter, gated by `HALCYON_WITH_OTEL`), `MockCliDriver` for DB-free unit tests.

**Design spec:** `docs/superpowers/specs/2026-06-24-halcyon-otel-context-propagation-design.md`

## Global Constraints

- **C++17 only.** No C++20 features.
- **No OpenTelemetry types in public headers.** `include/halcyon/**` must not `#include` any `opentelemetry/*` header; the parent context is opaque above the adapter (`src/observability/otel_adapter.cpp`).
- **No hard OTel dependency in core.** The adapter compiles only when `HALCYON_WITH_OTEL=ON` and `opentelemetry-cpp` is found; new `Tracer` methods have safe defaults so `NoopTracer` and existing mocks compile unchanged.
- **Zero overhead when no tracer is configured:** `has_tracer_ == false` ⇒ `captureContext` is never called and `instrument()` keeps its single-branch no-op fast path.
- **Warnings clean:** `-Wall -Wextra -Wpedantic`; run `clang-format` and `clang-tidy` before committing.
- **Spans always start and end on one thread** (sync: within one `instrument()` call; async: entirely on the worker). The design relies on this for scope/context stack consistency — do not move a live span object across threads.
- **Conventional commits:** `feat:`, `fix:`, `test:`, `docs:`, `build:`.
- **Naming:** files `lower_snake_case`; types `PascalCase`; public methods `camelCase`.

---

### Task 1: Seam interface — opaque context types + defaulted Tracer methods

**Files:**
- Modify: `include/halcyon/observability/tracing.hpp`
- Test: `tests/unit/test_observability.cpp`

**Interfaces:**
- Consumes: existing `obs::Span`, `obs::Tracer`, `obs::NoopTracer`, `obs::NoopSpan`.
- Produces:
  - `struct obs::SpanContext { virtual ~SpanContext() = default; };` — opaque parent handle.
  - `struct obs::ContextToken { virtual ~ContextToken() = default; };` — opaque RAII active-context token.
  - `Tracer::captureContext() -> std::shared_ptr<SpanContext>` (default `nullptr`).
  - `Tracer::startSpan(std::string_view, const SpanAttrs&, const SpanContext* parent) -> std::unique_ptr<Span>` (default delegates to 2-arg).
  - `Tracer::attachContext(const std::shared_ptr<SpanContext>&) -> std::unique_ptr<ContextToken>` (default `nullptr`).
  - `class obs::ScopedContext` — null-tolerant, movable RAII wrapper around `std::unique_ptr<ContextToken>`.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/test_observability.cpp` (after the existing includes/usings, e.g. near the other `TEST(Observability, ...)` blocks):

```cpp
TEST(Observability, NoopTracerContextDefaultsAreInert) {
    obs::NoopTracer t;
    EXPECT_EQ(t.captureContext(), nullptr);
    EXPECT_EQ(t.attachContext(nullptr), nullptr);
    auto span = t.startSpan("x", {}, nullptr);  // 3-arg overload exists
    ASSERT_NE(span, nullptr);                    // returns a NoopSpan, not null
    span->end();
    obs::ScopedContext empty;                    // default-constructed is falsy
    EXPECT_FALSE(static_cast<bool>(empty));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -S . -B build -DHALCYON_BUILD_TESTS=ON && cmake --build build -j 2>&1 | tail -20`
Expected: COMPILE FAIL — `no member named 'captureContext'` / `attachContext` / no 3-arg `startSpan` / `ScopedContext` not declared.

- [ ] **Step 3: Write minimal implementation**

In `include/halcyon/observability/tracing.hpp`, insert the two opaque structs *before* `struct Tracer` (right after `struct Span { ... };`) and extend `Tracer`:

```cpp
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
```

Then add `ScopedContext` immediately after the existing `class ScopedSpan { ... };`:

```cpp
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build -j && ctest --test-dir build -R NoopTracerContextDefaultsAreInert --output-on-failure`
Expected: PASS. Also confirm the whole suite still builds/passes: `ctest --test-dir build --output-on-failure`.

- [ ] **Step 5: Commit**

```bash
git add include/halcyon/observability/tracing.hpp tests/unit/test_observability.cpp
git commit -m "feat: add opaque SpanContext/ContextToken seam for trace context propagation"
```

---

### Task 2: Recording-tracer context modeling + nesting verification

This task upgrades the test doubles to model an active-context stack and span
self-activation (mirroring the real `OtelSpan`), then pins the nesting behavior
the core already produces given an activating tracer. No production code changes
— the real adapter fix lands in Task 5 and is verified there.

**Files:**
- Test: `tests/unit/test_observability.cpp`

**Interfaces:**
- Consumes: `obs::SpanContext`, `obs::ContextToken`, the 3-arg `startSpan`, `captureContext`, `attachContext` from Task 1.
- Produces (test-only, reused by Tasks 3–4):
  - `SpanRecord` gains `const void* parent = nullptr;`.
  - `struct RecordingContext : obs::SpanContext { const void* id; };`
  - Thread-local active-context stack `g_active_ctx`.
  - `RecordingTracer` overrides all three new `Tracer` methods; `RecordingSpan` self-activates.
  - `const SpanRecord* findChild(const RecordingTracer&, const std::string& name, const void* parent);`

- [ ] **Step 1: Write the failing tests**

Add these tests to `tests/unit/test_observability.cpp`:

```cpp
TEST(Observability, QuerySpanParentsAcquire) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(0);
    auto t = std::make_shared<RecordingTracer>();
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    cfg.observability.tracer = t;
    auto db = Database::open(driver, "X", cfg).value();

    ASSERT_TRUE(db.execute("UPDATE t SET a=?", 1).ok());

    const SpanRecord* exec = findSpan(*t, "halcyon.execute");
    ASSERT_NE(exec, nullptr);
    EXPECT_NE(findChild(*t, "halcyon.acquire", exec), nullptr);
}

TEST(Observability, TransactionSpanParentsAcquire) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    auto t = std::make_shared<RecordingTracer>();
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    cfg.observability.tracer = t;
    auto db = Database::open(driver, "X", cfg).value();

    auto r = db.transaction([](halcyon::Transaction& tx) {
        return tx.execute("INSERT INTO t VALUES (?)", 1);
    });
    ASSERT_TRUE(r.ok());

    const SpanRecord* txn = findSpan(*t, "halcyon.transaction");
    ASSERT_NE(txn, nullptr);
    EXPECT_NE(findChild(*t, "halcyon.acquire", txn), nullptr);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build -j 2>&1 | tail -20`
Expected: COMPILE FAIL — `SpanRecord` has no member `parent`; `findChild` not declared.

- [ ] **Step 3: Upgrade the recording doubles (the implementation)**

In `tests/unit/test_observability.cpp`, replace the existing `SpanRecord`,
`RecordingSpan`, `RecordingTracer`, and `findSpan` block with the context-aware
versions below, and add `RecordingContext`, the thread-local stack,
`RecordingContextToken`, and `findChild`:

```cpp
struct SpanRecord {
    std::string name;
    std::map<std::string, std::string> attrs;
    bool errored = false;
    const void* parent = nullptr;  // identity of the context this span started under
};

// Identity-only fake parent context for the recording tracer.
struct RecordingContext : obs::SpanContext {
    const void* id;
    explicit RecordingContext(const void* i) : id(i) {}
};

// Active-context stack for the current thread, modeling OTel RuntimeContext.
// Entries are span identities (a SpanRecord*) or an attached context id.
thread_local std::vector<const void*> g_active_ctx;

class RecordingSpan : public obs::Span {
public:
    explicit RecordingSpan(SpanRecord* r) : r_(r) {
        g_active_ctx.push_back(r_);  // self-activate (mirrors OtelSpan's scope)
    }
    void setAttribute(std::string_view k, std::string_view v) override {
        r_->attrs[std::string(k)] = std::string(v);
    }
    void setStatusError(std::string_view) override { r_->errored = true; }
    void end() override {
        if (ended_) return;
        ended_ = true;
        if (!g_active_ctx.empty() && g_active_ctx.back() == r_)
            g_active_ctx.pop_back();  // LIFO: ends on the same thread, reverse order
    }
    ~RecordingSpan() override { end(); }

private:
    SpanRecord* r_;
    bool ended_ = false;
};

class RecordingContextToken : public obs::ContextToken {
public:
    explicit RecordingContextToken(const void* id) : id_(id) {
        g_active_ctx.push_back(id_);
    }
    ~RecordingContextToken() override {
        if (!g_active_ctx.empty() && g_active_ctx.back() == id_)
            g_active_ctx.pop_back();
    }

private:
    const void* id_;
};

class RecordingTracer : public obs::Tracer {
public:
    std::unique_ptr<obs::Span> startSpan(std::string_view name,
                                         const obs::SpanAttrs& attrs) override {
        return startSpan(name, attrs, nullptr);
    }
    std::unique_ptr<obs::Span> startSpan(std::string_view name,
                                         const obs::SpanAttrs& attrs,
                                         const obs::SpanContext* parent) override {
        auto rec = std::make_shared<SpanRecord>();
        rec->name = std::string(name);
        for (const auto& [k, v] : attrs)
            rec->attrs[std::string(k)] = std::string(v);
        // Parent resolved BEFORE the span self-activates: explicit parent wins,
        // else the current active-context top (or none).
        rec->parent = parent
                          ? static_cast<const RecordingContext*>(parent)->id
                          : (g_active_ctx.empty() ? nullptr : g_active_ctx.back());
        {
            std::lock_guard<std::mutex> lk(mu);
            spans.push_back(rec);
        }
        return std::make_unique<RecordingSpan>(rec.get());
    }
    std::shared_ptr<obs::SpanContext> captureContext() override {
        return std::make_shared<RecordingContext>(
            g_active_ctx.empty() ? nullptr : g_active_ctx.back());
    }
    std::unique_ptr<obs::ContextToken> attachContext(
        const std::shared_ptr<obs::SpanContext>& ctx) override {
        const void* id =
            ctx ? static_cast<const RecordingContext*>(ctx.get())->id : nullptr;
        return std::make_unique<RecordingContextToken>(id);
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

const SpanRecord* findChild(const RecordingTracer& t, const std::string& name,
                            const void* parent) {
    std::lock_guard<std::mutex> lk(t.mu);
    for (const auto& s : t.spans)
        if (s->name == name && s->parent == parent) return s.get();
    return nullptr;
}
```

> Note: `<vector>` and `<mutex>` are already included by this file. Keep the
> existing `SpansCarrySystemAndStatement` / `ErrorPathMarksSpanErrored` tests —
> they continue to use `findSpan` and pass unchanged.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build -j && ctest --test-dir build -R "QuerySpanParentsAcquire|TransactionSpanParentsAcquire" --output-on-failure`
Expected: PASS. Also run the full suite: `ctest --test-dir build --output-on-failure` — all green.

- [ ] **Step 5: Commit**

```bash
git add tests/unit/test_observability.cpp
git commit -m "test: model active-context stack and span nesting in recording tracer"
```

---

### Task 3: `Database::useParentContext` explicit-parent guard

**Files:**
- Modify: `include/halcyon/database.hpp`
- Test: `tests/unit/test_observability.cpp`

**Interfaces:**
- Consumes: `tracer_` / `has_tracer_` members; `obs::ScopedContext`, `obs::SpanContext`, `obs::ContextToken` (Task 1); `RecordingContext`, `RecordingTracer`, `findSpan` (Task 2).
- Produces: `[[nodiscard]] obs::ScopedContext Database::useParentContext(std::shared_ptr<obs::SpanContext> ctx);`

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/test_observability.cpp`:

```cpp
TEST(Observability, UseParentContextParentsSyncSpans) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    auto t = std::make_shared<RecordingTracer>();
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    cfg.observability.tracer = t;
    auto db = Database::open(driver, "X", cfg).value();

    int marker = 0;
    auto parent = std::make_shared<RecordingContext>(&marker);
    {
        auto guard = db.useParentContext(parent);
        ASSERT_TRUE(static_cast<bool>(guard));  // tracer present -> real guard
        ASSERT_TRUE(db.execute("INSERT INTO t VALUES (?)", 1).ok());
    }

    const SpanRecord* exec = findSpan(*t, "halcyon.execute");
    ASSERT_NE(exec, nullptr);
    EXPECT_EQ(exec->parent, static_cast<const void*>(&marker));

    // Guard dropped -> the context is no longer active on this thread.
    auto after = t->captureContext();
    EXPECT_EQ(static_cast<RecordingContext*>(after.get())->id, nullptr);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -20`
Expected: COMPILE FAIL — `no member named 'useParentContext' in 'halcyon::Database'`.

- [ ] **Step 3: Write minimal implementation**

In `include/halcyon/database.hpp`, add the method in the public section, right
after the `queryAsync` method and before the `private:` label:

```cpp
    // Parent all Halcyon spans created on the current thread under `ctx` until
    // the returned guard is destroyed — synchronous calls and any async work
    // submitted while it is held. `ctx` comes from the OTel adapter
    // (obs::make_otel_active_context / obs::extract_otel_context). No-op (empty
    // guard) when no tracer is configured.
    [[nodiscard]] obs::ScopedContext useParentContext(
        std::shared_ptr<obs::SpanContext> ctx) {
        return obs::ScopedContext(
            has_tracer_ ? tracer_->attachContext(std::move(ctx))
                        : std::unique_ptr<obs::ContextToken>{});
    }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build -j && ctest --test-dir build -R UseParentContextParentsSyncSpans --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/halcyon/database.hpp tests/unit/test_observability.cpp
git commit -m "feat: add Database::useParentContext guard for explicit trace parent"
```

---

### Task 4: Async parent propagation across the executor boundary

**Files:**
- Modify: `include/halcyon/database.hpp`
- Test: `tests/unit/test_observability.cpp`

**Interfaces:**
- Consumes: `instrument(...)`, `executeAsync`, `queryAsync`, `captureContext`, `useParentContext` (Task 3), `RecordingContext`/`RecordingTracer` (Task 2).
- Produces: `instrument` gains a trailing `const obs::SpanContext* parent = nullptr`; `executeAsync`/`queryAsync` capture context at submit and forward it.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/test_observability.cpp`:

```cpp
TEST(Observability, QueryAsyncInheritsCallerContext) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"n"}, {{halcyon::detail::cli::Value{std::int64_t{7}}}}});
    auto t = std::make_shared<RecordingTracer>();
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    cfg.observability.tracer = t;
    auto db = Database::open(driver, "X", cfg).value();

    int marker = 0;
    auto parent = std::make_shared<RecordingContext>(&marker);
    std::future<halcyon::Result<std::vector<ObsRow>>> fut;
    {
        auto guard = db.useParentContext(parent);
        fut = db.queryAsync<ObsRow>("SELECT n FROM t");
    }  // guard drops here; the context was already captured at submit
    auto r = fut.get();
    ASSERT_TRUE(r.ok());

    const SpanRecord* q = findSpan(*t, "halcyon.query");
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->parent, static_cast<const void*>(&marker));
}
```

> `<future>` and `<vector>` are already transitively available via
> `halcyon/database.hpp`; add `#include <future>` to the test file if the build
> reports it missing.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ctest --test-dir build -R QueryAsyncInheritsCallerContext --output-on-failure`
Expected: FAIL — the async `halcyon.query` span runs on a worker thread with an
empty active stack, so `q->parent` is `nullptr`, not `&marker`.

- [ ] **Step 3: Write minimal implementation**

In `include/halcyon/database.hpp`:

(a) Add the trailing parameter to `instrument` and use the 3-arg `startSpan`.
Change the signature:

```cpp
    template <class Fn>
    static auto instrument(obs::MetricsSink* m, obs::Tracer* t, bool hasM,
                           bool hasT, std::string_view spanName,
                           const std::string& sql, Fn&& fn,
                           const obs::SpanContext* parent = nullptr)
        -> std::invoke_result_t<Fn> {
```

and the span construction inside it:

```cpp
        obs::ScopedSpan span =
            hasT ? obs::ScopedSpan(t->startSpan(
                       spanName,
                       {{"db.system", "db2"}, {"db.statement", sql}}, parent))
                 : obs::ScopedSpan();
```

(b) In `executeAsync`, capture the context on the calling thread and forward it:

```cpp
    template <class... Args>
    std::future<Result<std::int64_t>> executeAsync(const std::string& sql,
                                                   Args... args) {
        auto parent = has_tracer_ ? tracer_->captureContext()
                                  : std::shared_ptr<obs::SpanContext>{};
        return exec_->submit(
            [pool = pool_, attempts = default_attempts_,
             parent = std::move(parent), sql, args...]() {
                return instrument(
                    pool->metrics(), pool->tracer(), pool->metrics_enabled(),
                    pool->tracer_enabled(), "halcyon.execute", sql,
                    [&] {
                        return run_with_policy_on(
                            *pool, default_policy_for(*pool, attempts, sql),
                            [&](Connection& c) { return c.execute(sql, args...); },
                            pool->metrics(), pool->metrics_enabled());
                    },
                    parent.get());
            });
    }
```

(c) In `queryAsync`, the same capture + forward:

```cpp
    template <class T, class... Args>
    std::future<Result<std::vector<T>>> queryAsync(const std::string& sql,
                                                   Args... args) {
        auto parent = has_tracer_ ? tracer_->captureContext()
                                  : std::shared_ptr<obs::SpanContext>{};
        return exec_->submit(
            [pool = pool_, attempts = default_attempts_,
             parent = std::move(parent), sql, args...]() {
                return instrument(
                    pool->metrics(), pool->tracer(), pool->metrics_enabled(),
                    pool->tracer_enabled(), "halcyon.query", sql,
                    [&] {
                        return run_with_policy_on(
                            *pool, default_policy_for(*pool, attempts, sql),
                            [&](Connection& c) {
                                return c.template queryAs<T>(sql, args...);
                            },
                            pool->metrics(), pool->metrics_enabled());
                    },
                    parent.get());
            });
    }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build -j && ctest --test-dir build -R QueryAsyncInheritsCallerContext --output-on-failure`
Expected: PASS. Then the full suite: `ctest --test-dir build --output-on-failure`.

- [ ] **Step 5: Commit**

```bash
git add include/halcyon/database.hpp tests/unit/test_observability.cpp
git commit -m "feat: propagate caller trace context into async execute/query spans"
```

---

### Task 5: OTel adapter — self-activation, capture/attach/explicit-parent, header extraction

**Files:**
- Modify: `include/halcyon/observability/otel_adapter.hpp`
- Modify: `src/observability/otel_adapter.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/unit/test_otel_propagation.cpp`

**Interfaces:**
- Consumes: `obs::Tracer`, `obs::Span`, `obs::SpanContext`, `obs::ContextToken` (Task 1); `Database::useParentContext` (Task 3); async wiring (Task 4).
- Produces:
  - `std::shared_ptr<obs::SpanContext> obs::make_otel_active_context();`
  - `std::shared_ptr<obs::SpanContext> obs::extract_otel_context(const std::map<std::string, std::string>& headers);`
  - `OtelSpan` self-activates; `OtelTracer` implements `captureContext`, 3-arg `startSpan`, `attachContext`.

> This task requires `-DHALCYON_WITH_OTEL=ON` with `opentelemetry-cpp` (API +
> SDK + in-memory span exporter) installed. The test target builds only when the
> adapter is enabled.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_otel_propagation.cpp`:

```cpp
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
```

- [ ] **Step 2: Wire the OTel test target + run to verify it fails**

In `tests/CMakeLists.txt`, after the `gtest_discover_tests(halcyon_unit_tests)`
line and before the `if(HALCYON_BUILD_INTEGRATION_TESTS)` block, add:

```cmake
# OTel-gated propagation tests: real SpanId parent/child links via the in-memory
# exporter. Built only when the opentelemetry-cpp adapter is enabled.
if(HALCYON_OTEL_ADAPTER_ENABLED)
    add_executable(halcyon_otel_tests unit/test_otel_propagation.cpp)
    target_link_libraries(halcyon_otel_tests
        PRIVATE halcyon::halcyon GTest::gtest_main Threads::Threads
                opentelemetry-cpp::in_memory_span_exporter)
    target_include_directories(halcyon_otel_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/unit)
    gtest_discover_tests(halcyon_otel_tests)
endif()
```

Run:
```bash
cmake -S . -B build-otel -DHALCYON_BUILD_TESTS=ON -DHALCYON_WITH_OTEL=ON \
      -DCMAKE_PREFIX_PATH="$(brew --prefix opentelemetry-cpp)"
cmake --build build-otel -j 2>&1 | tail -25
```
Expected: COMPILE/LINK FAIL — `make_otel_active_context` / `extract_otel_context`
are not declared, and `OtelTracer` does not override the new methods.

- [ ] **Step 3: Implement the adapter**

In `include/halcyon/observability/otel_adapter.hpp`, add the includes and the two
helper declarations:

```cpp
#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "halcyon/observability/tracing.hpp"

namespace halcyon::obs {

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
```

Replace the body of `src/observability/otel_adapter.cpp` with:

```cpp
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
    void end() override { span_->End(); }

private:
    otel::nostd::shared_ptr<trace::Span> span_;
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
```

- [ ] **Step 4: Run the OTel tests to verify they pass**

Run:
```bash
cmake --build build-otel -j
ctest --test-dir build-otel -R OtelAdapter --output-on-failure
```
Expected: PASS (3 tests). Also rebuild/run the non-OTel suite to confirm no
regression: `cmake --build build -j && ctest --test-dir build --output-on-failure`.

- [ ] **Step 5: Commit**

```bash
git add include/halcyon/observability/otel_adapter.hpp \
        src/observability/otel_adapter.cpp \
        tests/CMakeLists.txt tests/unit/test_otel_propagation.cpp
git commit -m "feat: OTel adapter self-activates spans and propagates parent context"
```

---

### Task 6: Documentation — update parent design spec §9

**Files:**
- Modify: `docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md`

**Interfaces:**
- Consumes: the shipped behavior from Tasks 1–5.
- Produces: spec §9 documents context propagation and the caller entry points.

- [ ] **Step 1: Update the "Tracing spans" subsection**

In `docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md`, locate the
"### Tracing spans" subsection and append a "Context propagation" paragraph
immediately after the existing span/attribute list:

```markdown
### Context propagation

Halcyon spans propagate parent context so they form a correct trace:

- **Nesting:** a started span is the active span for its lifetime, so nested
  Halcyon spans (`halcyon.transaction` → `halcyon.query`/`halcyon.execute` →
  `halcyon.acquire`) and any user spans created inside a `transaction(fn)`
  callback parent correctly.
- **Async:** `executeAsync`/`queryAsync` capture the caller's active context at
  submit time and re-parent the worker-thread span under it, so async spans are
  not orphaned roots.
- **Explicit parent:** `Database::useParentContext(ctx)` returns a RAII guard
  that parents all Halcyon spans created on the current thread (sync and async)
  under `ctx`. Build `ctx` with the OTel adapter helpers
  `obs::make_otel_active_context()` or `obs::extract_otel_context(headers)` —
  the latter extracts W3C `traceparent`/`tracestate` to continue a distributed
  trace in a server handler. The parent context is opaque above the adapter
  (`obs::SpanContext`); no `opentelemetry-cpp` types appear in public headers.
```

- [ ] **Step 2: Verify the doc renders and references are accurate**

Run: `grep -n "Context propagation" docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md`
Expected: one match; confirm the surrounding section reads correctly and the
helper/method names match the shipped API (`useParentContext`,
`make_otel_active_context`, `extract_otel_context`).

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md
git commit -m "docs: document OpenTelemetry parent context propagation in spec §9"
```

---

## Final verification (per AGENTS.md)

After all tasks, run the full suite — unit, OTel-gated, and live-Db2 integration
— before claiming completion:

```bash
# Unit (no DB)
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON && cmake --build build -j
ctest --test-dir build --output-on-failure

# OTel-gated propagation tests
cmake -S . -B build-otel -DHALCYON_BUILD_TESTS=ON -DHALCYON_WITH_OTEL=ON \
      -DCMAKE_PREFIX_PATH="$(brew --prefix opentelemetry-cpp)"
cmake --build build-otel -j
ctest --test-dir build-otel -R OtelAdapter --output-on-failure

# Live Db2 integration (start container, set HALCYON_TEST_DSN) — see AGENTS.md.
# A Skipped integration result is NOT verification; bring up the container.
```
```bash
clang-format -i $(git diff --name-only --diff-filter=ACM | grep -E '\.(hpp|cpp)$')
```

## Self-Review

**Spec coverage:**
- Gap 1 (nesting) → Task 5 `OtelSpan` self-activation; verified by Task 2 (mock) + Task 5 (`NestedSpansShareTraceAndChain`).
- Gap 2 (async orphan) → Task 4 capture+forward; verified by Task 4 (mock) + Task 5 (`AsyncInheritsActiveSpanAcrossThreads`).
- Gap 3 (explicit parent) → Task 3 `useParentContext` + Task 5 `extract_otel_context`/`make_otel_active_context`; verified by Task 3 (mock) + Task 5 (`ExtractContinuesRemoteTrace`).
- Seam additions (§3) → Task 1. Adapter (§4) → Task 5. Core wiring (§5) → Tasks 3–4. Testing (§6) → Tasks 2–5. Docs (§7) → Task 6.
- Zero-overhead invariant → preserved in Task 4 (`has_tracer_` gate on capture; `instrument`'s `!hasM && !hasT` early return untouched).

**Placeholder scan:** none — every code step is complete, compile-ready code.

**Type consistency:** `SpanContext`/`ContextToken`/`ScopedContext`,
`captureContext()`, 3-arg `startSpan(..., const SpanContext*)`,
`attachContext(const std::shared_ptr<SpanContext>&)`, `useParentContext(...)`,
`make_otel_active_context()`, `extract_otel_context(const std::map<...>&)` are
used identically across Tasks 1–6. The mock's `RecordingContext::id` and
`SpanRecord::parent` identities line up with `findChild`/`findSpan` assertions.
