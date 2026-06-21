# Halcyon — Per-Connection Prepared-Statement LRU Cache — Design Spec

**Date:** 2026-06-21
**Status:** Proposed design (pre-implementation)
**Parent spec:** docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md (§Non-goals)

## 1. Overview

Promotes the deferred "per-connection prepared-statement LRU cache" non-goal
into a concrete feature. Today every `Connection::query`/`execute`/`queryAs`/
`executeBatch` call re-`prepare`s its SQL and finalizes the handle when the
transient `Statement` is destroyed. This feature keeps prepared handles alive on
the physical connection and reuses them, so repeated SQL on a pooled connection
skips the per-call prepare round-trip.

The cache is a **transparent optimization**: results and error semantics are
identical whether it is enabled or disabled. It changes only *how often* a
statement is prepared.

### Goals

- Reuse prepared `StatementHandle`s per physical connection, keyed by SQL text.
- Stay above the thin CLI seam: caching is portable core policy, unit-testable
  against `MockCliDriver`.
- Bounded memory: fixed-capacity LRU with deterministic eviction.
- Transparent to all existing call paths; on by default, one knob to disable.
- Correct under the existing single-active-cursor and reconnect semantics.

### Non-goals

- Cross-connection / global statement cache (the cache is per physical connection).
- SQL normalization or query rewriting beyond the existing named→positional pass.
- Caching across a physical reconnect (handles die with the connection).
- Server-side statement pooling / Db2 package cache tuning.

## 2. Decisions (locked)

| Topic | Decision |
|---|---|
| Activation | Transparent by default; `statementCacheSize = 0` disables |
| Cache key | Exact post-rewrite positional SQL string, used verbatim |
| Component | Dedicated `detail::StatementCache` in core, owned by `Connection` |
| Lending | RAII `StatementLease`; cached lease returns entry, transient finalizes |
| Busy-entry hit | Serve a transient uncached prepare (overflow); cached entry untouched |
| Capacity | `PoolConfig.statementCacheSize` count, default 64, LRU eviction finalizes |
| Invalidation | Clear all on reconnect; drop the single entry on a statement-level error |
| Seam change | Add `ICliDriver::closeCursor(StatementHandle)` |
| Observability | `halcyon_stmt_cache_total{result}` counter + `halcyon_stmt_cache_size` gauge |
| Thread-safety | None internal; single-owner connection, pool serializes handoff |

## 3. Architecture

Placement (unchanged seam invariant — only `closeCursor` is added below it):

```
facade ─► pool ─► core (Connection ─► StatementCache) ─► detail::cli ─► sqlcli1.h
```

`StatementCache` lives in `src/core/` and depends only on `ICliDriver`.

### 3.1 Seam addition

```cpp
// detail/cli/driver.hpp — new pure-virtual
// Closes any open cursor on stmt and resets it for re-bind/re-execute.
// Idempotent when no cursor is open. Maps to SQLFreeStmt(SQL_CLOSE).
virtual Result<void> closeCursor(StatementHandle stmt) = 0;
```

`Db2CliDriver` implements it via `SQLFreeStmt(hstmt, SQL_CLOSE)` (and
`SQL_RESET_PARAMS` as needed). `MockCliDriver` records the call and clears its
per-handle cursor state. This is the only seam change; it stays portable and
mockable.

### 3.2 StatementCache

```cpp
namespace halcyon::detail {

class StatementCache {
public:
  StatementCache(cli::ICliDriver& driver, cli::ConnectionHandle conn,
                 std::size_t capacity, observability::MetricsSink* metrics);
  ~StatementCache();  // finalizes all live handles

  // Returns a lease for sql: hit (reuse), miss (prepare+insert), or overflow
  // (transient prepare when the matching entry is busy or capacity == 0).
  Result<StatementLease> acquire(const std::string& sql);

  void clear();  // drop all entries WITHOUT finalize (used after reconnect)

private:
  struct Entry { std::string key; cli::StatementHandle handle; bool busy; };
  // LRU = std::list<Entry> (front = MRU) + unordered_map<string, list::iterator>.
  ...
};

}  // namespace halcyon::detail
```

Internals: an intrusive LRU (a `std::list<Entry>` with an
`unordered_map<std::string, iterator>` index). `capacity == 0` short-circuits to
always-transient. No mutex — see §6.

### 3.3 StatementLease

RAII borrow returned by `acquire`. Two modes:

- **Cached:** references the cache + its entry. On destruction: if not poisoned,
  `driver.closeCursor(handle)`, mark entry idle, splice to MRU. If poisoned,
  `driver.finalize(handle)` and erase the entry (re-prepared next time).
- **Transient (overflow / disabled):** owns the handle; on destruction
  `driver.finalize(handle)` unconditionally.

```cpp
class StatementLease {
public:
  cli::StatementHandle handle() const noexcept;
  void poison() noexcept;          // mark for drop on release (statement error)
  StatementLease(StatementLease&&) noexcept;
  ~StatementLease();               // release per mode above
  // non-copyable
};
```

### 3.4 Connection integration

`Connection` gains a `StatementCache` member, constructed from its driver,
handle, and the configured capacity. `prepare`-then-run sites are rewritten to
acquire a lease:

- `query(...)` → `acquire(sql)` → bind → execute → build a `ResultSet` that
  **owns the lease** (cursor lifetime). Lease released when the `ResultSet` dies.
- `execute(...)` / `execute_update` / `queryAs` / `collect` / `executeBatch` →
  `acquire(sql)`, use, release at end of call (they fully drain, no lingering
  cursor).

`ResultSet::owned_` changes from `std::optional<Statement>` to a holder that owns
a `StatementLease` (the transient lease subsumes the old finalize-on-destroy
`Statement` behavior). `Statement` as a standalone public type is retained for
direct `Connection::prepare`, which stays non-caching and returns a transient.

On a transparent reconnect (pool/Connection reconnect path), after the new
physical connection is established the Connection calls `cache.clear()`; the old
handles are already invalid, so entries are dropped without `finalize`.

## 4. Cache Key & Lifecycle

- **Key:** the exact string passed to `prepare` — i.e. the positional SQL after
  `detail::bind_named` rewrites named parameters. Anonymous-parameter SQL is used
  as-is. No trimming, casing, or normalization. Named and anonymous callers that
  resolve to identical prepared text share an entry.
- **Acquire outcomes:**
  - *miss* → `prepare`; if at capacity, evict LRU victim (`finalize`), insert new
    entry as MRU+busy; emit `result="miss"`.
  - *hit, idle* → mark busy, splice to MRU; emit `result="hit"`.
  - *hit, busy* → `prepare` a transient (no insert); emit `result="overflow"`.
  - *capacity 0* → always transient; emit `result="overflow"`.
- **Eviction** emits `result="evict"` and `finalize`s the victim handle.
- **Release** (cached, clean): `closeCursor`, mark idle. (cached, poisoned):
  `finalize` + erase. (transient): `finalize`.

## 5. Invalidation

- **Reconnect:** `clear()` — every entry dropped (no finalize; handles dead).
- **Statement-level error** (`bindParams`/`execute`/`fetch` returns an error on a
  cached handle): the lease is **poisoned**, so on release the entry is finalized
  and removed; the next call re-prepares it. Covers stale-plan / schema-change
  SQLSTATEs without flushing unrelated good entries.
- **Connection destruction:** `~StatementCache` finalizes all live handles
  (best-effort; broken-connection handles may already be dead).
- The cache **survives** pool checkout/return and transaction commit/rollback on
  the same physical connection (prepared statements outlive cursors and units of
  work in CLI). This is the intended warm-cache behavior with pooling.

## 6. Concurrency & Thread-Safety

`StatementCache` performs no internal locking. It is owned by a `Connection`,
which is single-owner and not shared across threads; `ConnectionPool` guarantees
a connection is lent to one thread at a time. The only same-connection overlap
possible — two unconsumed lazy `query()` cursors over the same SQL — is handled
by the busy→overflow rule (§4), not by locking.

## 7. Error Model

No new `ErrorCode`. The cache is transparent to `Result<T>`/throwing overloads:
a miss that fails to `prepare` returns the underlying prepare error exactly as
today; a statement error during use returns the same error and additionally
poisons the entry. `closeCursor` failures on release are swallowed (best-effort)
and the entry is dropped to avoid serving a half-reset handle.

## 8. Observability

Through the existing `MetricsSink` (no new interface; no-op when unconfigured):

- `halcyon_stmt_cache_total{result="hit"|"miss"|"evict"|"overflow"}` — counter
- `halcyon_stmt_cache_size` — gauge (current live entry count per connection)

Hit ratio = hit / (hit + miss + overflow). No new tracing spans; prepare cost
already appears inside the existing `halcyon.query`/`halcyon.execute` spans.

## 9. Configuration

`PoolConfig` gains:

```cpp
std::size_t statementCacheSize = 64;  // per-connection LRU capacity; 0 disables
```

Propagated to every connection the pool creates. Direct (non-pool) `Connection`
construction takes the same capacity (default 64).

## 10. Testing

**Unit (MockCliDriver):**

- miss→insert, hit→reuse (assert `prepare` called once across N identical calls).
- LRU eviction order at capacity; evicted handle `finalize`d.
- busy-entry overflow: second overlapping `query` on same SQL → transient prepare,
  cached entry count unchanged.
- poison-on-error: a forced `execute`/`fetch` error drops the entry; next call
  re-prepares.
- `clear()` on reconnect drops all entries without `finalize`.
- `closeCursor` invoked on cached-lease release; not on transient finalize path.
- `statementCacheSize = 0` → always transient, no entries retained.
- Named/anonymous SQL resolving to identical prepared text share one entry.
- Metrics: counter labels and gauge transitions.

**Integration (Dockerized Db2):**

- Repeated identical SELECT/DML on a pooled connection reuses the handle
  (observe reduced prepares / cache-hit metric).
- Forced connection-drop reconnect leaves the cache empty then re-warms.
- Cursor reuse correctness: SELECT, consume, re-execute same cached SELECT with
  new params returns correct rows (validates `closeCursor`).
- executeBatch repeated with identical SQL hits the cache.

## 11. Open Items

None blocking. `closeCursor` is the sole seam addition; metric names follow the
parent spec's §9 scheme.
