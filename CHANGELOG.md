# Changelog

All notable changes to Halcyon are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html) for its
public C++ API (see "Versioning & compatibility" in the README).

## Unreleased

## 1.2.0 - 2026-07-20

Coroutines & async streaming. The compiled library still targets C++17;
`halcyon/coro.hpp` is a C++20 opt-in header outside the `halcyon.hpp`
umbrella (toolchain floor for it: GCC 11+, Clang 14+, AppleClang 14+).

### Added
- C++20 coroutine layer (`halcyon/coro.hpp`, namespace `halcyon::coro`):
  dependency-free lazy `Task<T>` (move-only, symmetric transfer); awaitable
  `execute`, `query<T>`, `executeBatch`, and `transaction` (plain callable or
  `Task`-returning coroutine — commit/rollback always hop back to Halcyon
  workers); async streaming via `queryStreaming` → `StreamingQuery` with
  awaitable `next()` and chunked `LobReader` reads/drains; `syncWait` bridge.
  Executor-offload design: every op runs the existing instrumented sync path,
  so auto-retry, reconnect, metrics, tracing, and logging apply unchanged
  (one documented exception: a `Task`-returning transaction callback does not
  emit the `halcyon.transaction` span — see the coroutines guide).
- C++17 support surface: `Database::asyncBacking()` (pool-only sync view,
  weak executor state, call-site trace capture), stop-aware fire-and-forget
  `Executor::post`, `Executor::onWorkerThread()`, and a bind-only
  `TypeBinder` passthrough for seam `Value`s.
- Pool health stats: `PoolStats` snapshot via `ConnectionPool::stats()` /
  `Database::poolStats()` — internally consistent under a single lock.
- New guide `docs/guide/coroutines.md`; `examples/coro_orders.cpp`.

### Changed
- `Executor` teardown is now safe on its own worker threads (handle/state
  split: join-others, detach-self). `executeAsync`/`queryAsync` jobs each
  carry their own driver+pool keep-alive backing so queued work stays safe
  across teardown. Behavior-compatible.
- `docs/guide/async.md` no longer claims streaming cursors cannot cross
  threads; the documented contract is coordinated, never-concurrent use.

## 1.1.0 - 2026-07-13

Transactions & large data. Published as
[`v1.1.0`](https://github.com/YH-Hung/Halcyon/releases/tag/v1.1.0).

### Added
- Savepoints: RAII `Savepoint` guard (`Transaction::savepoint()`) with
  validated names, and savepoint-backed nested scopes
  (`Transaction::nested(fn)`).
- Isolation levels: `halcyon::Isolation` (Db2 terminology — UR/CS/RS/RR),
  pool-wide default (`PoolConfig::isolation`) and per-transaction overrides
  (`begin(iso)`, `transaction(iso, fn)`) restored on every exit path.
- LOB streaming: `queryStreaming` + `LobReader` chunked reads;
  `lobFile`/`lobStream`/`lobCallback` (+ `.asClob()`) data-at-exec writes.
  O(chunk) memory in both directions; positional binds; never auto-retried.
- Pluggable structured logging: `obs::ILogger` + `LogField` behind
  `ObservabilityConfig::logger`, with a dependency-free `StderrLogger`
  reference adapter. Zero overhead when unset.
- New error codes `InvalidArgument` and `InvalidState`.

### Changed
- For seam implementers only: `detail::cli::ICliDriver` gained five pure
  virtual methods (`setIsolation`, `fetchNext`, `getValue`, `getDataChunk`,
  `executeStreaming`). `detail/` is outside the public compatibility policy.

## 1.0.0 - 2026-07-01

First stable source tree. Source-consumption ready via `find_package(Halcyon)`
and CMake `FetchContent`. Published as
[`v1.0.0`](https://github.com/YH-Hung/Halcyon/releases/tag/v1.0.0).

### Added
- High-level `Database` facade with OO/fluent and functional free-function styles.
- Dual error model: `Result<T>` and throwing overloads; `Error` with `ErrorCode`,
  SQLSTATE, native SQLCODE, and a `retriable` flag.
- Named (`:name`) and anonymous (`?`) parameters; tuple and `HALCYON_REFLECT`
  struct row mapping; forward-only streaming `ResultSet`.
- Thread-safe `ConnectionPool` with validation, idle/lifetime reaping,
  transparent reconnect, and safe auto-retry of idempotent operations.
- Per-connection prepared-statement LRU cache.
- `std::future`-based async (`queryAsync`/`executeAsync`); coroutine-ready
  executor seam.
- RAII transactions (`ScopedTransaction`) and functional `transaction(...)`.
- Bulk insert via true Db2 CLI column-wise array binding (`executeBatch`,
  `batchOf`), with byte-budget chunking and a `Transaction::executeBatch` overload.
- Optional Prometheus metrics and OpenTelemetry tracing (with W3C context
  propagation), behind interfaces and CMake toggles; zero overhead when disabled.
- CMake package: `find_package(Halcyon)` -> `halcyon::halcyon`, and `FetchContent`
  consumption.
