# Halcyon — Modern C++17 DB2 Interface — Design Spec

**Date:** 2026-06-17
**Status:** Approved design (pre-implementation)

## 1. Overview

Halcyon is a modern C++17 client library for IBM Db2, built on the IBM Db2 CLI
(Call Level Interface, `sqlcli1.h`). It provides a high-level, ergonomic, and
type-safe API in both object-oriented/fluent and functional styles, with
first-class thread safety, native high concurrency, connection pooling,
transparent reconnect with safe auto-retry on transient failures, and optional
observability via `prometheus-cpp` (metrics) and `opentelemetry-cpp` (tracing).

### Goals

- Target **C++17**; embrace modern C++ idioms (RAII, value semantics, `optional`,
  `string_view`, `chrono`, traits) and best practices.
- **Thread-safe** by construction; safe for high-concurrency use.
- **Connection pooling** with validation, bounded growth, and lifecycle management.
- **Transparent reconnect** of dead connections; **safe auto-retry** of idempotent
  operations on transient DB failures.
- **High-level API:** "provide a connection string and you're done."
- **Named and anonymous** query parameters.
- **Ergonomic + type-safe** result mapping (tuples, named columns, opt-in struct
  reflection).
- **Dual error model:** `Result<T>` (functional) and throwing overloads (OO).
- **Optional observability** with zero hard dependencies in the core.
- Designed to be **coroutine-ready** (C++20 awaitables can be layered later
  without breaking the API).

### Non-goals (deferred to later versions)

- Per-connection prepared-statement LRU cache (single prepared statements are
  still core; only the *cache* is deferred).
- Large-result streaming / server-side cursors.
- LOB (BLOB/CLOB) streaming.
- Pluggable structured logging interface separate from OTel.
- Standalone pool health-stats accessor (pool stats are surfaced via metrics).
- Savepoints / nested transactions / explicit isolation-level API.
- Platforms beyond Linux x86_64 and macOS (no Windows/AIX in v1).

## 2. Decisions (locked)

| Topic | Decision |
|---|---|
| Underlying driver | IBM Db2 CLI (`sqlcli1.h`), vendored at `third_party/clidriver` |
| Platforms | Linux x86_64 + macOS |
| Build | CMake + `find_package` (consumer-provided deps) |
| Observability | Optional, behind interfaces, CMake toggles |
| Error model | Dual: `Result<T>` + throwing overloads |
| Row mapping | Tuple fetch + opt-in `HALCYON_REFLECT` struct mapping (+ range/iterator) |
| Concurrency | Sync thread-safe + `std::future` async now; coroutine-ready |
| Retry | Transparent reconnect always; auto-retry only safe/idempotent ops |
| Transactions | RAII guard + functional `transaction(...)` helper |
| Testing | Unit (mock CLI seam) + Dockerized Db2 integration (gated) |
| v1 extra scope | Bulk/array insert & batch execution |

## 3. Architecture (Approach A — Layered with thin CLI seam)

Dependency direction (`A ► B` = A depends on B):

```
facade ─► pool ─► core ─► detail::cli ─► sqlcli1.h
   │                          ▲
   └──► observability ────────┘   (interfaces only; adapters optional)
```

Layers:

1. **`detail::cli`** — the only code that includes `sqlcli1.h`. RAII wrappers over
   CLI handles (env/dbc/stmt) behind `ICliDriver` (mockable). Translates
   `SQLRETURN` + SQLSTATE into `halcyon::Error`.
2. **Core** — `Connection`, `Statement`, `ResultSet`, `Row`, parameter binding
   (named/anonymous), type mapping, transactions. Pure C++, depends only on the
   seam interface.
3. **Pool** — `ConnectionPool` (thread-safe, min/max, acquire timeout, validation,
   transparent reconnect + safe-retry) plus the async executor (internal thread
   pool returning `std::future`).
4. **Facade + Observability** — `Database` high-level entry, functional free
   functions, and observability hooks (`MetricsSink`/`Tracer` interfaces) with
   optional `prometheus`/`otel` adapters compiled in via CMake toggles.

**Invariant:** only files under `src/detail/cli/` and `cmake/FindDB2CLI.cmake` are
aware of IBM CLI specifics. Everything above the seam is portable, mockable C++17.

## 4. Project Layout

```
Halcyon/
├── CMakeLists.txt
├── cmake/                      # FindDB2CLI.cmake, package config, presets
├── third_party/clidriver/     # vendored IBM Db2 CLI driver (provided)
├── include/halcyon/
│   ├── halcyon.hpp             # umbrella header
│   ├── database.hpp            # facade: Database (high-level entry)
│   ├── connection.hpp          # Connection, Statement, ResultSet, Row
│   ├── pool.hpp                # ConnectionPool, PoolConfig
│   ├── transaction.hpp         # RAII guard + transaction(...) helper
│   ├── parameters.hpp          # named/anonymous param binding
│   ├── result.hpp              # Result<T> / expected type
│   ├── error.hpp               # Error, ErrorCode, SQLSTATE mapping, exceptions
│   ├── types.hpp               # type traits, tuple/struct mapping, HALCYON_REFLECT
│   ├── async.hpp               # std::future API + executor, coroutine hooks
│   └── observability/
│       ├── metrics.hpp         # MetricsSink interface
│       └── tracing.hpp         # Tracer/Span interface
├── src/
│   ├── detail/cli/             # ICliDriver + Db2CliDriver (only sqlcli1.h user)
│   ├── core/                   # connection/statement/resultset/binding impl
│   ├── pool/                   # pool + reconnect/retry + executor
│   ├── facade/                 # database impl, free functions
│   └── observability/          # prometheus_adapter.cpp, otel_adapter.cpp (guarded)
├── tests/
│   ├── unit/                   # mock ICliDriver
│   └── integration/            # Dockerized Db2 (CTest label "integration")
├── examples/                   # functional + OO usage
└── docker/                     # db2 compose for integration tests
```

## 5. Public API Surface

Every operation is reachable in OO/fluent and functional/free-function form, each
with dual error handling (`Result<T>` + throwing overloads).

### Setup (connection string → ready)

```cpp
auto db = halcyon::Database::open(
    "DATABASE=SAMPLE;HOSTNAME=h;PORT=50000;UID=u;PWD=p;",
    halcyon::PoolConfig{.min = 2, .max = 16, .acquireTimeout = 5s});
// Result<Database>; or Database::openOrThrow(...) for throwing style.
```

### Query — anonymous params (positional `?`)

```cpp
auto rows = db.query("SELECT id, name FROM users WHERE age > ? AND city = ?", 21, "NYC");
for (auto& row : rows.value()) {              // range/iterator support
    auto [id, name] = row.as<int, std::string>();
}
```

### Query — named params

```cpp
auto rows = db.query("SELECT id, name FROM users WHERE age > :age",
                     halcyon::params{{"age", 21}});
```

### Struct mapping (opt-in reflection)

```cpp
struct User { int id; std::string name; };
HALCYON_REFLECT(User, id, name);
auto users = db.queryAs<User>("SELECT id, name FROM users"); // Result<std::vector<User>>
```

### Functional style

```cpp
using namespace halcyon;
auto users = query_as<User>(db, "SELECT id, name FROM users WHERE id = :id",
                            params{{"id", 7}});
```

### Async (`std::future`, coroutine-ready)

```cpp
std::future<Result<std::vector<User>>> f = db.queryAsync<User>("SELECT ...");
```

### Transactions (both styles)

```cpp
// functional: commit on success, rollback on throw/error-Result
db.transaction([&](Transaction& tx) {
    tx.execute("INSERT INTO a VALUES (?)", 1);
    tx.execute("UPDATE b SET x=? WHERE id=?", 2, 3);
});

// RAII: auto-rollback at scope exit unless committed
auto tx = db.begin();
tx.execute(/* ... */);
tx.commit();
```

### Bulk / batch (v1 in-scope)

```cpp
db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batchOf(rowsVector));
```

## 6. Result & Error Model

### `Result<T>`

A lightweight, vendored `expected`-style type (C++17). Holds `T` or `Error`.

```cpp
template <class T> class Result {
  bool ok() const; explicit operator bool() const;
  T& value() &;                      // throws Exception if error
  const Error& error() const;
  template <class F> auto map(F&&);      // Result<U>
  template <class F> auto and_then(F&&); // Result<U>
  template <class F> auto or_else(F&&);
  T value_or(T fallback) const;
};
```

`.value()` throwing is the bridge that powers the throwing overloads: the
throwing API calls the `Result` API and unwraps.

### `Error`

```cpp
struct Error {
  ErrorCode   code;        // enum: Connection, Timeout, Constraint, Syntax,
                           //       Deadlock, Transient, Mapping, Pool, Unknown
  std::string sqlstate;    // raw 5-char SQLSTATE (e.g. "08001")
  int         nativeError; // SQLCODE / native db2 code
  std::string message;     // from SQLGetDiagRec
  bool        retriable;   // transient classes → drives auto-retry
};
```

SQLSTATE → `ErrorCode` mapping (in `detail::cli`):

| SQLSTATE / code | ErrorCode | retriable |
|---|---|---|
| `08xxx` (connection) | Connection | yes |
| `-30081` (comm lost) | Transient | yes |
| `40001` (deadlock/rollback) | Deadlock | yes |
| `57033`/lock timeout | Timeout | yes |
| `23xxx` (constraint) | Constraint | no |
| `42xxx` (syntax/access) | Syntax | no |
| other | Unknown | no |

### Throwing hierarchy (OO users)

`halcyon::Exception` (base, carries `Error`) → `ConnectionException`,
`QueryException`, `ConstraintException`, `TimeoutException`,
`TransientException`, `MappingException`.

## 7. Type Mapping

A `TypeBinder<T>` traits layer maps C++ ↔ Db2 CLI C types, bidirectionally
(parameter bind + column fetch).

| C++ type | Db2 CLI C type |
|---|---|
| `int32_t` / `int64_t` | `SQL_C_SLONG` / `SQL_C_SBIGINT` |
| `double` / `float` | `SQL_C_DOUBLE` / `SQL_C_FLOAT` |
| `std::string` / `std::string_view` | `SQL_C_CHAR` (UTF-8) |
| `bool` | `SQL_C_BIT` |
| `std::optional<T>` | nullable; `NULL` ↔ `nullopt` (length indicator) |
| `std::chrono::system_clock::time_point` | TIMESTAMP (UTC, via `SQL_C_CHAR` ISO-8601) |
| `halcyon::Date` / `Time` / `Timestamp` | DATE / TIME / TIMESTAMP (string-exact, `SQL_C_CHAR`) |
| `std::vector<std::byte>` | `SQL_C_BINARY` (chunked; embedded NULs preserved) |
| `halcyon::Decimal` | `SQL_C_CHAR` decimal (exact) |

Rules:

- **Temporal carriage is text (`SQL_C_CHAR`), not the CLI `*_STRUCT` types.** Db2
  round-trips DATE/TIME/TIMESTAMP losslessly as character data, and the neutral
  seam `Value` deliberately carries no temporal alternative, so a chrono/decimal
  value crossing the seam needs zero new variant members. `std::chrono::system_clock::time_point`
  ↔ TIMESTAMP is the one clean mapping under **C++17** (which has no calendar
  types); bare DATE and TIME have no epoch/clock, and TIMESTAMP can carry more
  fractional precision than `system_clock`, so the string-exact `halcyon::Date`/
  `Time`/`Timestamp` wrappers are provided for full fidelity and for DATE/TIME.
  `time_point` values are normalised to UTC.

- **Nullability is type-safe:** a non-`optional` column returning NULL yields a
  `Mapping` error, never silent garbage.
- **Compile-time checks:** `row.as<...>()` and `queryAs<T>` `static_assert` column/
  field arity and that each type has a `TypeBinder`.
- **`HALCYON_REFLECT(T, fields...)`** expands to a field-tuple description used by
  both the binder and the struct mapper. No RTTI, no external reflection lib.

## 8. Pool, Concurrency, Reconnect & Retry

### `ConnectionPool`

- `PoolConfig`: `min`, `max`, `acquireTimeout`, `idleTimeout`, `maxLifetime`,
  `validationQuery` (default `SELECT 1 FROM SYSIBM.SYSDUMMY1`), `validateOnAcquire`,
  reconnect backoff (`baseDelay`, `maxDelay`, `maxAttempts`).
- Internals: mutex + condition_variable guarding an idle deque + active count.
- `acquire()` returns a `PooledConnection` RAII handle that returns the connection
  on destruction (or discards if marked broken).
- Lazy growth `min`→`max`; blocks up to `acquireTimeout`, then returns `Pool`/
  `Timeout` error.
- Background maintenance reaps connections past `idleTimeout`/`maxLifetime` and
  refills to `min`.

### Validation & transparent reconnect

- On acquire (if `validateOnAcquire`) or on a connection-class error
  (`08xxx`/`-30081`) during use, the connection is marked broken, physically
  reconnected with bounded exponential backoff, and never returned poisoned.

### Safe auto-retry (default policy)

- Auto-retry happens **only if** `error.retriable` **and** the operation is
  classified safe: connect/validate, read-only (`SELECT`/`WITH…SELECT`), or an
  explicitly idempotent execute (`ExecPolicy::idempotent()`).
- Non-idempotent writes outside a transaction: reconnect happens, but the call
  returns the error (no replay); the caller decides.
- Anything inside an open `Transaction` is **never** silently retried — the whole
  transaction must be re-driven.
- Per-call `ExecPolicy` (`maxAttempts`, `backoff`, `idempotent`) overrides the
  default when callers know better.

### Async executor

- `async.hpp` exposes `queryAsync`/`executeAsync` → `std::future<Result<T>>`,
  backed by an internal fixed-size thread pool (default size = pool `max`).
- Single `submit(callable) -> future` chokepoint so a C++20 coroutine
  `task<T>`/awaitable layer can wrap the same executor later without API breakage.

### Thread-safety contract

- `Database` and `ConnectionPool`: fully thread-safe; share freely.
- A single `Connection` / `Transaction` / `ResultSet`: not for concurrent use by
  multiple threads (one logical owner at a time), matching CLI handle semantics.
  The pool guarantees a connection is handed to only one thread at a time.

## 9. Observability

Core defines pure interfaces and emits events; adapters compile only when toggled.

```cpp
struct MetricsSink {                       // observability/metrics.hpp
  virtual void counter(std::string_view, double, const Labels&) = 0;
  virtual void histogram(std::string_view, double, const Labels&) = 0;
  virtual void gauge(std::string_view, double, const Labels&) = 0;
};
struct Tracer {                            // observability/tracing.hpp
  virtual std::unique_ptr<Span> startSpan(std::string_view, const SpanAttrs&) = 0;
};
```

- **Default:** no-op sinks (zero overhead, nothing linked).
- **`HALCYON_WITH_PROMETHEUS=ON`** builds `PrometheusMetricsSink` over `prometheus-cpp`.
- **`HALCYON_WITH_OTEL=ON`** builds `OtelTracer` over `opentelemetry-cpp`.

### Metrics (final names)

- `halcyon_queries_total{op,status}` — counter
- `halcyon_query_duration_seconds{op}` — histogram
- `halcyon_pool_connections{state="idle"|"active"}` — gauge
- `halcyon_pool_acquire_wait_seconds` — histogram
- `halcyon_reconnects_total` — counter
- `halcyon_retries_total{outcome}` — counter
- `halcyon_errors_total{code}` — counter

### Tracing spans

`halcyon.query`, `halcyon.execute`, `halcyon.transaction`, `halcyon.acquire`,
`halcyon.reconnect`, with attributes `db.system=db2`, `db.statement` (optionally
redacted), `db.rows_affected`, `error.sqlstate`.

### Context propagation

Halcyon spans propagate parent context so they form a correct trace:

- **Nesting:** a started span is the active span for its lifetime, so nested
  Halcyon spans (`halcyon.transaction` → `halcyon.query`/`halcyon.execute` →
  `halcyon.acquire`) and any user spans created inside a `transaction(fn)`
  callback parent correctly. A `halcyon.reconnect` span emitted during acquisition
  likewise nests under the enclosing `halcyon.acquire` span.
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

## 10. Build & Packaging

- CMake (min 3.20). Targets: `halcyon::halcyon` (shared + static). C++17.
- Warnings: `-Wall -Wextra -Wpedantic`; `HALCYON_WARNINGS_AS_ERRORS` optional.
- `cmake/FindDB2CLI.cmake` defaults to `third_party/clidriver`, overridable via
  `DB2_CLIDRIVER_ROOT`; links `db2`; bakes RPATH to its `lib/` so examples/tests
  run without manual `DYLD_LIBRARY_PATH`/`LD_LIBRARY_PATH`. Linux deploys drop a
  Linux `clidriver` at the same path.
- Options: `HALCYON_WITH_PROMETHEUS`, `HALCYON_WITH_OTEL`, `HALCYON_BUILD_TESTS`,
  `HALCYON_BUILD_EXAMPLES`, `HALCYON_WARNINGS_AS_ERRORS`.
- Installs headers + CMake package config (`find_package(Halcyon)` →
  `halcyon::halcyon`).

## 11. Testing & Tooling

- **Unit** (no DB): GoogleTest with a `MockCliDriver` implementing `ICliDriver`.
  Covers binding, type mapping, error classification, pool logic
  (acquire/timeout/reconnect/retry decisions), and the `Result` monad. Runs in CI
  on all platforms.
- **Integration** (real DB): `docker/` compose with `icr.io/db2_community/db2`,
  CTest label `integration` (off by default). Covers real queries, named/anon
  params, struct mapping, transactions, bulk insert, forced connection-drop
  reconnect, and (when built) metrics/trace emission.
- **CI:** Linux + macOS build matrix; unit tests always; integration gated job.
- **Tooling:** `clang-format`, `clang-tidy`, optional ASan/TSan/UBSan presets
  (TSan especially valuable for the pool).

## 12. Open Items

None blocking. Metric names finalized in §9.
