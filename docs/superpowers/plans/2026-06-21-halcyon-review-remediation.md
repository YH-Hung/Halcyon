# Halcyon — Code-Review Remediation Plan

**Date:** 2026-06-21
**Status:** In progress
**Trigger:** External code review of the foundation/core/pool/facade implementation.

Addresses the four review findings (2×P1, 2×P2) plus the three flagged spec gaps.
Everything is TDD (failing test first) and verified against the full suite
including the live-Db2 integration tests (AGENTS.md mandate — never accept a
`Skipped` integration result as verification).

## Findings (done)

1. **P1 — Db2CliDriver not thread-safe.** All pooled connections share one
   `Db2CliDriver`; its `conns_`/`stmts_` maps + `nextConn_`/`nextStmt_` counters
   were mutated without a lock. Fix: a `std::mutex mu_` held only around the
   bookkeeping (look up `SQLHSTMT`/`SQLHDBC` into a local, release before the CLI
   round-trip) so distinct connections still run concurrently. The slow
   `SQLDriverConnect` runs unlocked (the dbc is thread-private until published).
   Verification: existing `Db2PoolIntegration.ConcurrentAcquireAndQuery` (24
   concurrent prepare/execute/fetch/finalize over 6 threads) under TSan + live
   Db2. *(code complete; TSan/live verification batched into final step.)*

2. **P1 — async use-after-free.** `executeAsync`/`queryAsync` captured raw
   `this`; a destroyed `Database` copy dangled while the shared executor ran the
   task. Fix: factor the run logic into static `*_on(ConnectionPool&, …)`
   helpers and have the async lambdas capture `pool_` + `default_attempts_`
   (never `this`, never `exec_` — `exec_` would create an executor self-cycle).
   Verified RED→GREEN under AddressSanitizer
   (`DatabaseAsync.TaskOutlivesLaunchingDatabaseCopy`).

3. **P2 — iteration drops fetch errors.** `ResultSet::iterator::advance()`
   collapsed fetch errors into end-of-cursor. Fix: store the `Error` on the
   `ResultSet`, expose `ResultSet::ok()`/`error()` (forwarded by `QueryResult`),
   and split the advance branches. Verified RED→GREEN
   (`ResultSetTest.MidStreamFetchErrorIsSurfacedNotSilentlyDropped`).

4. **P2 — named-param scanner corrupts SQL.** `bind_named()` rewrote `:name`
   inside literals. Fix: literal-aware scan skipping `'…'`/`"…"` (doubled-quote
   escapes), `--` line comments, `/* */` block comments. Verified RED→GREEN
   (5 new `Parameters.*` tests).

## Spec gaps (planned — design via parallel scout, then TDD)

5. **Type mapping.** Real binary reads in `getColumn` (`SQL_BINARY/VARBINARY/BLOB`
   → `SQL_C_BINARY` bytes, not string); `halcyon::Decimal` exact type + binder
   (string-carriage, spec §7 = `SQL_C_CHAR`); `std::chrono` date/time binders
   (ISO string-carriage — the neutral `Value` variant has no temporal member).
6. **Observability.** `observability/metrics.hpp` (`MetricsSink`+`Labels`+Noop) and
   `tracing.hpp` (`Tracer`/`Span`+Noop) per spec §9; injectable, default no-op;
   emit the spec's metrics/spans from the facade/pool; `src/observability/*`
   adapters gated by `HALCYON_WITH_PROMETHEUS`/`HALCYON_WITH_OTEL`.
7. **Packaging/install.** Install lib + headers + export set; generate/install
   `HalcyonConfig.cmake` + version file; install `FindDB2CLI.cmake` with
   `find_dependency(DB2CLI)`; smoke `find_package(Halcyon)` consumer test.

## Verification protocol (final step)

- Full unit suite (normal build) — 100% pass.
- Unit suite under ASan — clean (async lifetime).
- Live Db2 via Docker (`HALCYON_BUILD_INTEGRATION_TESTS=ON`, `HALCYON_TEST_DSN`,
  `ctest -L integration`) — never `Skipped`. Plus the concurrency test under TSan.
- Tear down the container afterward; leave the working tree clean.
- Commits only when the user asks.
