# Changelog

All notable changes to Halcyon are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html) for its
public C++ API (see "Versioning & compatibility" in the README).

## Unreleased

## 1.0.0 - 2026-07-01

First stable source tree. Source-consumption ready via `find_package(Halcyon)`
and CMake `FetchContent`. No GitHub Release or `v1.0.0` tag has been published
yet; consumers should pin `main`, a reviewed commit, or a release tag once one
exists.

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
