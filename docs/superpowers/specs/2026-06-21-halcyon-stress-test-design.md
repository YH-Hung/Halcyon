# Halcyon — Concurrency Stress & Performance Test Suite — Design Spec

**Date:** 2026-06-21
**Status:** Proposed design (pre-implementation)
**Parent spec:** docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md (§8 Concurrency, §11 Testing)

## 1. Overview

Halcyon claims to be "thread-safe by construction" and "safe for high-concurrency
use" (parent spec §1, §8). Today that claim is backed by unit tests that exercise
pool logic single-threaded plus one light live-Db2 test
(`ConcurrentAcquireAndQuery`, 24 tasks / 6 threads). Nothing stresses lock
contention, the statement cache, reconnect-under-load, the maintenance/reaper
race, or measures throughput.

This feature adds a dedicated stress and performance suite that verifies two
distinct properties:

1. **Correctness under concurrency** — no data races, deadlocks, use-after-free,
   poisoned-handle handoff, lost tasks, or pool-accounting drift under heavy
   multi-threaded load. Assertion-based, deterministic, CI-gated, run under
   ThreadSanitizer / AddressSanitizer.
2. **Performance under concurrency** — throughput (ops/sec), latency percentiles,
   and scaling across thread/pool sizes. Reported numbers, with a few soft sanity
   gates on the fake backend; never a hard CI threshold.

These are delivered as **two front-ends over one shared workload-runner core**.

### Goals

- Prove the parent spec's thread-safety contract (§8) holds under stress, with
  evidence (sanitizer-clean runs + invariant assertions), not assertion-free
  "it didn't crash" runs.
- Measure and make visible whether the pool/executor actually parallelize work
  rather than serializing on internal mutexes.
- Stay above the thin CLI seam: the suite depends only on public Halcyon headers
  plus `ICliDriver` (which tests already implement, cf. `MockCliDriver`).
- Require **zero production code changes**; this is purely additive test
  infrastructure.
- Keep the default build and the default `ctest` run fast — the suite is opt-in.

### Non-goals

- Replacing the existing unit suite or the live integration suite.
- Benchmarking Db2 server performance (the live backend is for realism, not for
  judging the database).
- A third-party benchmarking dependency (e.g. Google Benchmark) — the project
  keeps deps lean; the shared runner provides the statistics we need.
- Hard performance regression gates that fail CI (deferred; soft gates only).
- Coroutine/awaitable stress (coroutines remain a future layer per parent spec).

## 2. Decisions (locked)

| Topic | Decision |
|---|---|
| Deliverables | Two: a correctness suite (assertions, sanitizers) and a perf harness (numbers) |
| Shared core | One `WorkloadRunner` + `ConcurrentFakeDriver` + scenario functors drive both |
| Correctness backend | Thread-safe fake driver only (no DB); bounded by iteration count |
| Perf backend | Fake **and** live Db2 (`--backend=fake\|live`); live reads `HALCYON_TEST_DSN` |
| Fake driver | New dedicated `ConcurrentFakeDriver`; `MockCliDriver` left untouched |
| Scenarios | All six: pool contention, executor saturation, statement cache, reconnect/fault, transaction churn, lifecycle/reaper races |
| Perf metrics | Throughput + p50/p95/p99/max latency + scaling sweep across thread/pool sizes |
| Perf gating | Report + soft sanity gates on the **fake** backend only; live is report-only |
| Correctness build | CTest label `stress` (opt-in); `HALCYON_SANITIZER=thread\|address\|undefined` |
| Perf form | Standalone `halcyon_stress` executable; not a default CTest test |
| Build gate | `HALCYON_BUILD_STRESS_TESTS` CMake option, default OFF |

## 3. Architecture (Approach A — shared runner core, two front-ends)

A new top-level `tests/stress/` tree, built only when `HALCYON_BUILD_STRESS_TESTS`
is ON.

```
tests/stress/
├── CMakeLists.txt
├── support/
│   ├── concurrent_fake_driver.hpp   # thread-safe ICliDriver (§4)
│   ├── latency_histogram.hpp        # per-thread fixed-bucket histogram, merged at end
│   ├── workload_runner.hpp/.cpp     # barrier-synced N-thread runner + RunReport (§5)
│   └── workloads.hpp                # the 6 scenario functors, shared by both front-ends (§6)
├── correctness/                     # GoogleTest target ── front-end #1
│   └── test_stress_concurrency.cpp  # one TEST per scenario; assertion-based; TSan/ASan-clean
└── perf/                            # standalone binary ── front-end #2
    └── halcyon_stress_main.cpp      # scaling sweep + report; fake or live backend
```

Dependency direction: everything in `tests/stress/` sits above the seam and
depends only on public headers (`halcyon/pool.hpp`, `connection.hpp`, `async.hpp`,
`database.hpp`, `transaction.hpp`) plus `detail/cli/driver.hpp` (the `ICliDriver`
interface). The two front-ends share the `support/` core; the only difference is
their stop condition, their backend, and whether they assert (correctness) or
time and report (perf).

**Why this shape:** the dangerous, hard-to-review concurrent orchestration code
(thread spawn, barrier, join, error capture) is written once in `WorkloadRunner`
and audited once. Each scenario is a small functor with no thread management of
its own. This serves the two-deliverables requirement directly: same workloads,
two front-ends.

## 4. `ConcurrentFakeDriver`

A standalone `ICliDriver` implementation purpose-built for concurrent access.
Unlike `MockCliDriver`'s FIFO scripting (`resultSets`/`execRowCounts` popped per
call, inherently order-dependent), **every response is derived deterministically
from its inputs**, so any number of threads may call it with no ordering
assumptions.

### 4.1 Handle model

- `connect()` returns a fresh `ConnectionHandle` from an atomic counter and records
  the handle in a live-connection set (mutex-guarded).
- `prepare(conn, sql)` returns a fresh `StatementHandle` that privately remembers
  its owning `conn` and `sql`. This lets the driver detect the bug we most want to
  catch: **a statement handle used after its connection was reconnected/closed**
  (poisoned-handoff) → returns an error rather than silently succeeding.
- `disconnect()` removes the connection from the live set; subsequent use of its
  statements is detectable.

### 4.2 Canned results (no scripting)

- `execute`/`columnCount`/`fetch`/`getColumn` synthesize a result purely from the
  prepared SQL. Convention:
  - SQL matching `SELECT <int> ...` yields exactly one row, one column = `<int>`.
  - SQL matching a small known set used by the cache/txn scenarios maps to a known
    grid.
  - DML (`INSERT`/`UPDATE`/`DELETE`) returns a fixed affected-row count (e.g. 1).
- Because the answer is a function of the SQL, each workload knows the **expected**
  value for every call and asserts it — this is how correctness is proven, not
  merely the absence of a crash.
- Per-handle cursor position lives in a `std::unordered_map<StatementHandle, …>`
  guarded by a mutex. Handles from different threads/connections never alias
  (counter is atomic and monotonic), but the map container itself is shared, so it
  is locked.

### 4.3 Realism knobs (atomically configurable)

- `queryLatency` — optional `sleep_for` inside `execute`/`fetch` to simulate I/O so
  contention is realistic rather than instantaneous.
- `connectLatency` — separate, typically larger, latency for `connect` (the
  expensive operation a pool exists to amortize).

### 4.4 Fault injection (drives the reconnect scenario)

- `failConnectEvery`, `failExecuteEvery`, `killConnectionEvery` — atomic "1-in-N"
  counters. A tripped counter returns a **retriable** `Error` (SQLSTATE `08001` or
  native `-30081`) or marks a connection dead so the next `isAlive` returns false.
- Faults are deterministic by call count, so any failure is reproducible given the
  same configuration and seed.

### 4.5 Counters & thread-safety

- `std::atomic` counters: `connectCalls`, `disconnectCalls`, `prepareCalls`,
  `executeCalls`, `closeCursorCalls`, `aliveCalls`, plus a peak-concurrent-in-flight
  gauge (incremented/decremented around `execute`).
- Counters are atomic; the handle/cursor/live-connection maps are mutex-guarded.
  The fake is intentionally a little pessimistic (one mutex around its maps) — it
  is test infrastructure, and it still leaves the *real* contention where we want
  it: in Halcyon's pool and executor, not in the fake.

## 5. `WorkloadRunner` & measurement

The shared engine both front-ends drive; the single audited home for thread
orchestration.

### 5.1 Contract

```cpp
struct Stop {                     // iteration- or duration-bounded
  std::optional<std::size_t> total_iters;       // correctness: deterministic
  std::optional<std::chrono::milliseconds> duration;  // perf: steady-state
};

struct RunConfig {
  std::size_t threads;
  Stop stop;
  std::size_t warmup_iters = 0;   // perf only; excluded from stats
  std::uint64_t seed = 0;         // per-thread RNG = seed ^ thread_index
};

struct Workload {
  std::string name;
  std::function<void(WorkerCtx&)> body;          // one unit of work, called in a loop
  std::function<void(const RunReport&)> verify;  // correctness assertions (optional)
};

RunReport run(const Workload&, const RunConfig&);
```

`WorkerCtx` exposes the thread index, a per-thread RNG, the thread-local latency
histogram, and a way to record an error/assertion failure.

### 5.2 Execution model

1. Spawn `threads` workers, each with its own RNG and its own latency histogram
   (no shared per-op state → no false contention, no measurement skew).
2. **Barrier:** all workers block on a hand-rolled C++17 gate (mutex +
   condition_variable; `std::latch` is C++20) until every thread has started, then
   release together, so we measure steady-state contention rather than
   thread-startup stagger.
3. Each worker loops calling `workload.body(ctx)`, timing each call into its
   thread-local histogram, until the stop condition is met (iteration count split
   across threads, or wall-clock duration elapsed).
4. Join all workers; merge histograms; assemble the `RunReport`.
5. **Error propagation:** the first failed assertion / unexpected `Result` error in
   any worker is captured and surfaced by the runner. In the correctness front-end
   this becomes a GoogleTest failure; nothing is swallowed.

### 5.3 `RunReport`

Carries: workload name, thread count, total ops, wall time, throughput (ops/sec),
merged latency p50/p95/p99/max, error count by `ErrorCode`, and the driver's
end-of-run counters (connects, reconnects, cache hits/misses/overflows, peak
in-flight). Correctness `verify` callbacks assert on these fields; the perf
front-end formats them into a report row.

### 5.4 Measurement honesty

- Timing uses `std::chrono::steady_clock`.
- The latency histogram is fixed-bucket log-scale, allocation-free on the hot path,
  thread-local, merged only at the end — so the measurement does not itself create
  contention or distort the numbers.
- In perf mode, `warmup_iters` (or a warmup duration) run before timing starts and
  are discarded.

## 6. Scenarios (workloads + invariants)

Each scenario is a `Workload` (a `body` plus a `verify`). The correctness target
runs all six fake-only with iteration bounds and the `verify` assertions; the perf
harness reuses the same `body` for timing. Each is an individual `TEST` so a
failure localizes to one scenario.

**Entry point per scenario.** Auto-retry and the safe-retry policy live in the
`Database` facade (`run_with_policy_on`/`query_impl_on`), **not** in raw
`ConnectionPool::acquire` + `Connection::query`. The pool itself only does
*in-place* transparent reconnect on the `validateOnAcquire` path; replay of a
retriable statement error is a facade concern. Scenarios therefore choose their
entry point to match what they verify: scenarios that target the pool/executor
mechanics in isolation drive the raw `ConnectionPool`/`Executor`; scenarios that
verify the product paths (auto-retry recovery, async, transaction-lease lifecycle)
drive a shared `Database` handle (copyable, pass-by-value across threads). The
backend driver is the same `ConcurrentFakeDriver` either way, injected via
`Database::open(shared_ptr<ICliDriver>, dsn, cfg)`.

### 6.1 Pool acquire/release contention
Raw `ConnectionPool` (pool mechanics in isolation). `threads` ≫ `pool.max` (e.g. 64
vs `max=8`), short `acquireTimeout`. Body: `acquire()` → trivial `SELECT 1` →
release via RAII scope.
*Invariants:* peak concurrent leases ≤ `max`; `idle + active == total` at end;
every acquire either succeeds or fails **only** with `ErrorCode::Pool` (timeout) —
never a crash or a double-handed connection; after the run `idle == total &&
active == 0`.

### 6.2 Async executor saturation
Flood the async path with far more tasks than worker threads, via both
`Database::executeAsync`/`queryAsync` (the product path: shared `Executor`,
pool-capturing tasks) and the lower-level `async_with_connection`/`Executor::submit`.
Body: submit a batch and collect futures.
*Invariants:* every future resolves exactly once; tasks-in == results-out (no
dropped or duplicated tasks); no deadlock at executor destruction with a non-empty
queue; tasks that outlive the launching `Database` copy still complete (the
shared-executor / captured-pool lifetime contract).

### 6.3 Statement-cache correctness under reuse
Driven through a shared `Database` with `statementCacheSize > 0` and a small `max`
so the same physical connection and its cache are reused; many threads issue a small
rotating set of SQL strings, including the same SQL concurrently (forcing the
busy→overflow path) and lazy unconsumed cursors.
*Invariants:* every query returns the value the fake encoded in its SQL (proves no
cross-thread handle/row mixups); `hit + miss + overflow == executes`; live cache
size ≤ `statementCacheSize`; no use-after-finalize (the fake's conn/sql tagging
flags it otherwise).

### 6.4 Reconnect + transient-fault injection under load
Driven through a shared `Database` handle (so the facade's safe-retry policy is in
play), with fault knobs on (`killConnectionEvery`, `failExecuteEvery` with
retriable SQLSTATEs) and `validateOnAcquire = true` (so the pool's in-place
reconnect path also fires). The backoff `sleep` is stubbed to a no-op so faults
churn fast. Full workload running.
*Invariants:* the run completes without hang or crash; reconnects/retries actually
occurred (`reconnects > 0` and/or `halcyon_retries_total > 0` — proves the path was
exercised, not a no-op pass); no statement handle from a dead connection is ever
reused (the fake's conn/sql tagging errors otherwise); read-only statements that hit
a retriable error are transparently recovered per the safe-retry policy (parent spec
§8), while non-idempotent writes surface the error without silent replay; final pool
invariants hold (`idle == total && active == 0`).

### 6.5 Transaction churn
Driven through a shared `Database` handle: concurrent `begin`/`transaction(...)`
across pooled connections, mixing commit and rollback (so `ScopedTransaction`'s
lease-return / `markBroken` lifecycle is exercised). Body: open a transaction, do
1–2 ops, randomly commit or roll back.
*Invariants:* autocommit toggled off/on symmetrically per transaction; `commits +
rollbacks == transactions started`; no transaction leaks a connection (final
`active == 0`); no connection used by two transactions at once.

### 6.6 Pool lifecycle races
`startMaintenanceThread = true` with a tiny `maintenanceInterval` and aggressive
`idleTimeout`/`maxLifetime` so the reaper fires constantly while acquires/releases
race it; plus a teardown variant that destroys the pool with work still in flight.
*Invariants:* the reaper never reaps a leased (active) connection; `total` never
drops below `min` except transiently during reconnect; gauge snapshots never read a
negative active count; clean destruction with in-flight tasks (no use-after-free —
the marquee TSan/ASan target).

Under TSan/ASan the **absence of sanitizer reports** across all six is the core
"no concurrency issues" evidence; the `verify` assertions are the "works as
expected" evidence.

## 7. Performance harness (`halcyon_stress`)

A standalone executable; not registered with CTest by default.

### 7.1 CLI

| Flag | Meaning | Default |
|---|---|---|
| `--backend=fake\|live` | fake driver, or live Db2 (`HALCYON_TEST_DSN`) | `fake` |
| `--scenario=pool\|executor\|cache\|reconnect\|txn\|lifecycle\|all` | which workload(s) | `all` |
| `--threads=8,16,32,64` | comma list → scaling sweep | `1,2,4,8,16` |
| `--pool-max=N` | pool max size | `8` |
| `--duration=5s` / `--iters=N` | stop condition | `5s` |
| `--warmup=1s` | discarded warmup | `1s` |
| `--latency=<µs>` | fake per-call latency | `0` |
| `--seed=N` | RNG seed | `0` |
| `--format=table\|csv` | output format (csv for diff/plot) | `table` |
| `--strict` | soft gates set the exit code | off |

For each `(scenario, thread-count)` cell the harness runs the workload via the
shared runner and prints a row: threads, pool-max, throughput (ops/sec),
p50/p95/p99/max latency, error count, reconnects, cache hit-rate. The scaling sweep
makes the headline question visible: *does throughput rise as threads/connections
increase, or does contention flatten it?*

If `--backend=live` and `HALCYON_TEST_DSN` is unset, the harness exits with a clear
error message (it does not silently fall back to fake).

### 7.2 Soft gates (fake backend only)

Printed as `PASS`/`WARN`; they affect the exit code only under `--strict`, so they
never accidentally break CI:

- **Scaling:** throughput at `max=8, threads=16` ≥ ~1.5× throughput at `threads=2`
  (proves the pool parallelizes rather than serializing on its mutex). Multiplier
  configurable.
- **No starvation:** in the contention scenario, the acquire-timeout error rate
  stays below a configurable ceiling at a generous timeout.
- **Latency sanity:** p99 within a configurable multiple of p50 (catches
  lock-convoy / pathological tail behavior).

Live-backend numbers are **report-only** (DB/network latency dominates; gating
them would be noise).

## 8. Build, tooling & CI

- **`HALCYON_BUILD_STRESS_TESTS`** (default OFF). When ON, builds the
  `halcyon_stress_tests` GoogleTest target (CTest label `stress`) and the
  `halcyon_stress` executable.
- **`HALCYON_SANITIZER=thread|address|undefined|""`** (default empty). When set,
  applies `-fsanitize=<kind>`, `-fno-omit-frame-pointer`, and `-g` to the stress
  targets (and, optionally, the existing unit tests). ThreadSanitizer is the
  headline configuration for this feature.
- The `stress` CTest label is opt-in (`ctest -L stress`) and never part of the
  default `ctest` run, so the normal unit suite stays fast.
- **Docs:** a short `tests/stress/README.md`, plus an `AGENTS.md` addition
  documenting the canonical invocation:

  ```bash
  cmake -S . -B build -DHALCYON_BUILD_TESTS=ON \
        -DHALCYON_BUILD_STRESS_TESTS=ON -DHALCYON_SANITIZER=thread
  cmake --build build -j
  ctest --test-dir build -L stress --output-on-failure   # TSan-clean correctness
  ./build/tests/stress/halcyon_stress --scenario=all --threads=1,2,4,8,16  # perf report
  ```

- **CI shape** (described, not necessarily wired in the first cut): a dedicated,
  opt-in job builds the stress suite under TSan and runs `ctest -L stress`. The
  perf binary is run manually / nightly for trend tracking, not on every PR.

## 9. Testing this test code

The suite is itself concurrent code, so it gets a minimal layer of meta-checks:

- A tiny self-test that the `ConcurrentFakeDriver` returns the SQL-encoded value
  for a known query and that its fault counters trip at the configured rate (so a
  green correctness run cannot be a false pass from a fake that never faults).
- A `verify`-fails-loudly check: a deliberately wrong expectation in one workload,
  run once during development, confirms the runner surfaces worker assertion
  failures as test failures (then removed/guarded).
- The correctness target is expected to be run under TSan in CI; running it without
  a sanitizer is still valid (asserts invariants) but is not sufficient evidence of
  race-freedom on its own.

## 10. Open Items

None blocking. Hard performance-regression gating and full CI wiring of a nightly
perf trend are deferred (§1 non-goals); the soft gates and the opt-in TSan job are
the first cut.
