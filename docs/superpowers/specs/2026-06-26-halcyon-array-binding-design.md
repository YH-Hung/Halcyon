# Halcyon — True Db2 CLI Array Binding for `executeBatch` — Design Spec

**Date:** 2026-06-26
**Status:** Approved design (pre-implementation)
**Parent spec:** docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md (§5 Bulk / batch)

## 1. Overview

The design spec lists "Bulk/array insert & batch execution" as v1 in-scope
(§"v1 extra scope", §5 "Bulk / batch (v1 in-scope)"). The shipped
implementation does **not** match that promise: `Connection::executeBatch`
(`include/halcyon/connection.hpp`) loops `bindParams` + `execute` **once per
row** ("executes it for each row of positional params"), and the user guide
(`docs/guide/batch-operations.md`) documents exactly that — "calling execute
once per row." There is no Db2 CLI array binding (`SQL_ATTR_PARAMSET_SIZE`,
column-wise parameter arrays) anywhere in the tree.

This feature replaces the per-row loop with **true Db2 CLI array binding**: a
parameter set of many rows is bound column-wise and sent to the server in a
single `SQLExecute` per chunk, collapsing N network round-trips and N commits
(under autocommit) into a handful. It honors the spec's v1 promise instead of
silently downgrading it.

The change is a **transparent under-the-hood upgrade**. The public API —
`Batch`, every `batchOf` overload, and both `executeBatch` signatures
(`Connection` and `Database`) — is byte-for-byte unchanged. Only *how* the rows
reach Db2 changes, plus a documented shift in the failure contract (§7).

### Goals

- Bind a row-set column-wise and execute it via Db2 CLI array binding below the
  thin CLI seam, keeping everything above the seam portable and mockable.
- Preserve the public `executeBatch` return contract: `Result<std::int64_t>` =
  total rows affected on success.
- Bound client memory for large batches via byte-budget chunking.
- Keep the existing scalar `bindParams`/`execute` path untouched (used by
  single-row `execute`/`query`).

### Non-goals (v1)

- Per-row success/failure reporting (which rows failed, partial counts). The
  failure contract is single-`Error` (§7); richer reporting is YAGNI for v1 and
  can be added later without breaking this contract.
- Server-side bulk-load utilities (`LOAD`, `IMPORT`). This is CLI array binding
  through `SQLExecute`, nothing more.
- Cross-row type coercion/promotion (§6 requires column homogeneity).
- Public knobs for chunk size or paramset size — internal for v1.

## 2. The seam change

Array binding is inherently CLI-specific (`SQLSetStmtAttr` +
`SQL_ATTR_PARAMSET_SIZE`), so it must live **below** `ICliDriver` — it cannot be
expressed in the portable layer without leaking `sqlcli1.h`. We add **one** new
method to `ICliDriver` (`include/halcyon/detail/cli/driver.hpp`) that binds and
executes a row-set atomically:

```cpp
// Binds `rows` as a column-wise parameter array and executes it via Db2 CLI
// array binding, chunking internally to bound memory. Returns the total number
// of rows affected, summed across chunks. Precondition: `rows` is non-empty and
// rectangular (every row has identical arity). On any row failure, returns a
// single Error carrying the Db2 diagnostic (SQLSTATE); the chunk is atomic (§7).
virtual Result<std::int64_t> executeBatch(
    StatementHandle stmt,
    const std::vector<std::vector<Value>>& rows) = 0;
```

Why one combined bind+execute method rather than a separate `bindParamsArray` +
reuse of `execute`: column-wise buffers and the paramset attribute state must
live together from bind through execute, and byte-budget chunking means looping
bind+execute internally. A combined method keeps buffer lifetime, the paramset
attributes, and chunking entirely inside the driver as one atomic operation —
no half-bound state crosses the seam. It is also the smallest seam growth (one
method) and leaves the scalar path (`bindParams`/`execute`) fully intact.

## 3. Db2 driver implementation

In `src/detail/cli/db2_cli_driver.cpp`, `Db2CliDriver::executeBatch`:

### 3.1 Column type resolution

For each column, scan rows top-to-bottom; the column's bind C/SQL type comes
from the **first non-`Null`** `Value`. NULLs are carried by the indicator array,
not by the type. An all-`Null` column binds as `SQL_C_CHAR`/`SQL_VARCHAR` with
every indicator `SQL_NULL_DATA` (mirrors the scalar null path at
db2_cli_driver.cpp:159). Type→CLI mapping is identical to the scalar
`bindParams` switch (db2_cli_driver.cpp:159–192): `bool`→`SQL_C_BIT`/`SQL_BIT`,
`int64`→`SQL_C_SBIGINT`/`SQL_BIGINT`, `double`→`SQL_C_DOUBLE`/`SQL_DOUBLE`,
`string`→`SQL_C_CHAR`/`SQL_VARCHAR`, `bytes`→`SQL_C_BINARY`/`SQL_BINARY`.

### 3.2 Mixed-type guard (validated above the seam)

Rectangularity and column type-homogeneity are **portable** properties of the
`Value` variants, independent of any CLI type. They are therefore validated in
the portable layer (`Connection::executeBatch`, §4) *before* the seam call, so
the check is unit-testable against `MockCliDriver`. If a non-null element in a
column is a different `Value` variant alternative than the column's resolved
type, the portable layer returns an `Error` with code `ErrorCode::Mapping` (the
existing code for value↔column type mismatches) naming the offending column and
row index, and the driver is never called. Reflected-struct and tuple `batchOf`
columns are homogeneous by construction, so this only catches hand-built
malformed `Batch` objects. No coercion is attempted. The driver (§3.1) assumes
validated, non-empty, rectangular, homogeneous input and resolves each column's
CLI type from its first non-null value.

### 3.3 Column-wise buffers

- **Fixed-width columns** (`int64`/`double`/`bit`): one `chunkRows × sizeof(T)`
  contiguous array, element `i` at offset `i*sizeof(T)`.
- **Variable-width columns** (`string`/`binary`): one `chunkRows × maxLen`
  buffer where `maxLen` is the longest element *in that chunk*, element `i` at
  offset `i*maxLen`.
- Each column gets a parallel `SQLLEN[chunkRows]` length/indicator array holding
  each element's actual byte length, or `SQL_NULL_DATA` for nulls.

### 3.4 Byte-budget chunking

Accumulate rows into a chunk, tracking estimated bound bytes (sum of per-column
buffer contributions for the rows so far). When adding the next row would push
the chunk over the budget (`kBatchByteBudget`, default ~16 MiB), flush the
current chunk first. A chunk always contains **at least one row**, so a single
row wider than the budget still goes through as its own chunk. This bounds
*client* memory; it does not bound server log/lock pressure (§8).

Per chunk:

1. `SQLSetStmtAttr(SQL_ATTR_PARAMSET_SIZE, chunkRows)`
2. `SQLSetStmtAttr(SQL_ATTR_PARAM_BIND_TYPE, SQL_PARAM_BIND_BY_COLUMN)`
3. `SQLSetStmtAttr(SQL_ATTR_PARAM_STATUS_PTR, statusArray)` —
   `SQLUSMALLINT[chunkRows]`
4. `SQLSetStmtAttr(SQL_ATTR_PARAMS_PROCESSED_PTR, &processed)` — `SQLULEN`
5. one `SQLBindParameter` per column (array buffer + indicator array)
6. one `SQLExecute`
7. `SQLRowCount` → aggregate affected count for the chunk; add to running total

After the final chunk, reset `SQL_ATTR_PARAMSET_SIZE` to 1 and clear the status/
processed pointers so the cached statement is clean for later scalar reuse.

### 3.5 Row-count aggregation

`SQLRowCount` returns the total affected across the chunk's parameter set on
Db2; the method returns the sum across all chunks. This preserves the existing
public contract ("total rows inserted").

## 4. Portable layers

- `Connection::executeBatch` (connection.hpp:372) drops its per-row loop and
  delegates to the single seam call. The empty-batch short-circuit (`return 0`)
  stays in the portable layer so the driver's precondition (non-empty) holds.
  Before delegating, it validates the batch is rectangular and each column is
  type-homogeneous ignoring NULLs (§3.2), returning `ErrorCode::Mapping` on
  violation without calling the driver — keeping the check portable and
  unit-testable via `MockCliDriver`. Statement-poisoning on a driver error is
  preserved.
- `Database::executeBatch` (database.hpp:367) is **unchanged** — it still leases,
  delegates, and discards a broken connection on `ErrorCode::Connection`.
- **New:** `Transaction::executeBatch` (transaction.hpp) and
  `ScopedTransaction::executeBatch(const Batch&)` (database.hpp) are added as
  forwarders, mirroring the existing `execute`/`query` forwarders. Without them
  the §7 "batch inside a transaction" atomicity pattern is not expressible
  through the facade. These are additive — no existing signature changes.

## 5. Mock + tests

- `MockCliDriver` (tests/unit/mock_cli_driver.hpp) gains the new `executeBatch`:
  it records the full row-set for assertions and returns a scripted total (or a
  scripted/queued `Error`). Existing scalar recording is untouched.
- `tests/unit/test_database_batch.cpp` moves from per-row-execute expectations to
  the single array call. New unit cases:
  - rectangular row-set binds and returns the scripted total,
  - NULL elements within a column,
  - all-NULL column,
  - mixed-type column → `Error` naming column/row index,
  - multi-chunk aggregation (sum of per-chunk counts),
  - empty batch → `0` (no seam call).
- `tests/integration/test_db2_roundtrip.cpp` (live, opt-in): a large multi-chunk
  insert with verified affected count + read-back, and a constraint-violation
  batch asserting the `Error` is classified (SQLSTATE 23505) and the failed
  chunk is atomic (nothing committed).

## 6. Type homogeneity (summary)

A column is bound with one CLI type. NULLs are free (indicator array). Non-null
elements in a column must share one `Value` alternative; otherwise the call
fails fast with a clear `Error`. No promotion, no per-row fallback. This is a
deliberate v1 constraint that the reflected/tuple `batchOf` builders satisfy by
construction.

## 7. Failure & atomicity contract

True array binding sends a whole parameter set in one `SQLExecute`; Db2 attempts
the set and, on error, rolls the whole chunk back, so the old "stop at the first
failing row" is no longer literally true. The v1 contract:

- **Return type unchanged:** `Result<std::int64_t>` — total rows affected on
  success.
- **Any row failure → a single classified `Error`.** The driver surfaces the
  Db2 diagnostic (SQLSTATE → `ErrorCode`, e.g. 23505 → `Constraint`). It does
  **not** name the failing row: measured against Db2 11.5.9.0, an array-bind
  failure leaves the param-status array at `SQL_PARAM_DIAG_UNAVAILABLE`, the
  diagnostics carry `SQL_NO_ROW_NUMBER`, and `PARAMS_PROCESSED` is the full
  chunk count — so the row index is not recoverable from the driver, and
  deriving it by replay is unsafe (it would commit prior rows under autocommit,
  or roll back the caller's work inside a transaction). No per-row result
  vector (non-goal).
- **Each chunk is atomic.** A failed chunk commits nothing (verified live), so
  under autocommit the batch up to the failing chunk is applied and the failing
  chunk is wholly rolled back.
- **Atomicity is the caller's, via a transaction.** Wrap the batch in
  `begin()` → `tx.executeBatch(...)` → `commit()` for all-or-nothing; on error,
  roll back and re-drive the whole batch. This matches the existing "no
  auto-retry — caller re-drives on failure" note at connection.hpp:372 /
  database.hpp:367. (Requires the new `ScopedTransaction::executeBatch`, §4.)

## 8. Performance & large batches

True array binding is performance-positive on the common path; the contract in
§7 steers callers onto the *faster* path, not a slower one. (This section is
static analysis from Db2 CLI semantics + the codebase; §9 adds the empirical
check.)

- **Autocommit dominates, and it favors transactions.** With autocommit ON (the
  default; no transaction) every `SQLExecute` forces a synchronous commit — a
  transaction-log flush. Array binding already collapses N per-row executes into
  `⌈bytes / kBatchByteBudget⌉` chunk executes, so commits drop from N to
  #chunks even without a transaction. Wrapping the batch in a transaction drops
  it to exactly **one** commit for the whole load (`SQLEndTran` at
  transaction.hpp:73). "Transaction-for-atomicity" therefore *removes* log
  forces; it is the standard Db2 bulk-insert pattern.
- **The status/error machinery is free on the success path.**
  `SQL_ATTR_PARAM_STATUS_PTR`/`PARAMS_PROCESSED_PTR` are pointers Db2 fills
  during execute; writing N status words is trivial beside the inserts, and the
  `SQLUSMALLINT[chunkRows]` status array is tiny beside the column buffers. The
  status array is only *read* on the error path (an O(chunkRows) scan).
- **"Re-drive on error" cost is bounded by error rate ≈ 0** for a write path.
  Inside a transaction a failed batch rolled back, so re-driving the whole batch
  is correct (nothing to skip). Frequent partial failures on huge batches are
  the niche the deferred per-row-reporting option serves.

**Caveat — log/lock pressure on very large all-or-nothing batches.** Because all
chunks of a transaction-wrapped `executeBatch` commit together, a multi-million-
row batch holds row locks and accumulates log records until the single commit,
risking lock escalation or `SQLOGFULL`. Byte-budget chunking bounds *client
memory*, not server log/lock pressure. Mitigation (no API change): for very
large loads, slice into several independently-committed `executeBatch` calls,
trading strict whole-load atomicity for bounded log use. Documented in the batch
guide.

## 9. Empirical validation

Add a batch-insert throughput benchmark to
`tests/stress/perf/halcyon_stress_main.cpp` (via the existing
`tests/stress/support/workload_runner.hpp`) comparing, against live Db2:

- per-row execution vs. array binding,
- autocommit-on vs. transaction-wrapped.

Run during the release-readiness live-runs phase to put numbers on §8 and catch
any surprise.

## 10. Documentation impact

- Design spec §5 "Bulk / batch": keep v1 in-scope, but describe it as true CLI
  array binding with the §7 failure contract.
- `docs/guide/batch-operations.md`: replace "calling execute once per row" with
  the array-binding description; add a "Performance & large batches" note (§8
  mitigation).
- `README.md`: align any per-row wording with array binding.
