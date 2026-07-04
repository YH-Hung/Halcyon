# Halcyon — Db2 CLI Rowset (Block) Fetch for the Read Path — Design Spec

**Date:** 2026-06-28
**Status:** Approved design (pre-implementation)
**Parent spec:** docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md (§5 Query, §7 Type Mapping)
**Sibling:** docs/superpowers/specs/2026-06-26-halcyon-array-binding-design.md (the write-path mirror)

## 1. Overview

Halcyon's write path was upgraded to true Db2 CLI **array binding**
(`executeBatch`, §sibling): N rows are bound column-wise and sent in one
`SQLExecute` per chunk. The **read path is still row-at-a-time** and is, in fact,
the more expensive of the two:

- `Db2CliDriver::fetch` (db2_cli_driver.cpp:438) issues **one `SQLFetch` per
  row**.
- `Db2CliDriver::getColumn` (db2_cli_driver.cpp:449) issues, **for every single
  cell**, a `SQLDescribeCol` *and* a `SQLGetData`. The column is re-described on
  every read.

So materializing an `R`-row × `C`-column result costs `R` fetches plus `R·C`
describes plus `R·C` get-datas — the describes are pure waste, repeated per row.

This feature introduces Db2 CLI **rowset (block) fetch**: columns are bound once
into column-wise array buffers, `SQL_ATTR_ROW_ARRAY_SIZE` is set to a block of
`R` rows, and a single `SQLFetch` fills the whole block. It is the exact
read-side mirror of array binding, and it unifies *both* read paths
(materializing `queryAs<T>`/`collect` and streaming `ResultSet`/`Row`) onto one
seam primitive.

The change is a **transparent under-the-hood upgrade**. The public API — `query`,
`queryAs`, `Row::as<>`, the `ResultSet` range/iterator, the functional
`query_as`, and async variants — is byte-for-byte unchanged. Only *how* rows
reach the portable layer changes.

### Goals

- Fetch result rows in blocks via Db2 CLI rowset binding below the thin CLI seam,
  keeping everything above the seam portable and mockable.
- Unify the materializing and streaming read paths onto **one** seam method.
- Preserve every public read contract: identical `Value`/type fidelity, identical
  forward-only/single-cursor/move-only `ResultSet` semantics, identical `Mapping`
  and connection-error behavior.
- Bound client memory for wide/large result sets via a fixed byte budget.
- Eliminate the per-cell `SQLDescribeCol` waste on both the fast path and the
  fallback path (describe once, cache).

### Non-goals (this iteration)

- Per-call or global tuning knobs for block size — internal fixed budget for now
  (mirrors the write path's `kBatchByteBudget`). Revisit if a real need appears.
- Hybrid bind-plus-`SQLGetData` for result sets that mix bounded and long
  columns. Long/LOB columns trigger a clean **per-statement fallback** (§5); the
  hybrid optimization is deferred.
- Scrollable/positionable cursors (`SQLFetchScroll` with absolute/relative
  positioning). We fetch forward only.
- Server-side cursor tuning (`SQL_ATTR_CURSOR_TYPE`, block-cursor row-count
  pragmas) beyond setting the rowset array size.
- Column-by-name row access — orthogonal ergonomics, not part of this change.

## 2. The seam change

Rowset fetch is inherently CLI-specific (`SQLBindCol`, `SQL_ATTR_ROW_ARRAY_SIZE`,
`SQL_ATTR_ROWS_FETCHED_PTR`), so it must live **below** `ICliDriver`. We replace
the row-at-a-time read pair (`fetch` + `getColumn`) as the portable layer's read
primitive with **one** block method on `ICliDriver`
(`include/halcyon/detail/cli/driver.hpp`):

```cpp
// Fetches up to maxRows rows from the open cursor on stmt as a row-major block
// of neutral Values. Returns:
//   - a non-empty block of [1, maxRows] rows on success (a short block, < maxRows,
//     is normal and does NOT signal end-of-cursor),
//   - an empty block exactly when the cursor is exhausted (clean end),
//   - a classified Error on a driver failure (e.g. a mid-stream connection drop).
// The driver describes the result columns once (cached on the statement), then
// uses rowset binding when every column is bounded, or an internal row-at-a-time
// fallback when any column is long/LOB (see the design spec §5). Either path
// returns byte-identical Values. The statement handle is left clean (columns
// unbound, rowset attributes reset) for later cache reuse.
virtual Result<std::vector<std::vector<Value>>> fetchBlock(
    StatementHandle stmt, std::size_t maxRows) = 0;
```

**Why one combined describe+bind+fetch+transpose method** rather than exposing
`bindCols` + `fetch` + `getBlock` separately: the column-wise buffers, the
indicator arrays, and the rowset attribute state must live together from bind
through fetch, and the bounded-vs-fallback decision is a CLI detail. A combined
method keeps all buffer lifetime and CLI state inside the driver as one
operation, leaks nothing across the seam, and is the smallest portable surface
(one method) — symmetric with the write path's single `executeBatch`.

**Removed from the seam:** `fetch(StatementHandle)` and
`getColumn(StatementHandle, index)` are removed from `ICliDriver` once nothing
above the seam references them. Their Db2 implementations survive as **private
helpers** inside `Db2CliDriver` — they back the §5 fallback path. `columnCount`
and `columnName` remain (used by the portable layer and tests).

## 3. Db2 driver implementation

In `src/detail/cli/db2_cli_driver.cpp`, `Db2CliDriver::fetchBlock`:

### 3.1 Describe-once column metadata

`StmtState` (db2_cli_driver.cpp:546) gains a cached, lazily-populated column
description, cleared whenever the cursor is closed or the statement is
re-executed:

```cpp
struct ColMeta {
    SQLSMALLINT sqlType;     // from SQLDescribeCol
    SQLSMALLINT cType;       // bind C type (SQL_C_SBIGINT/DOUBLE/BIT/CHAR/BINARY)
    SQLULEN     declWidth;   // declared column size in bytes (char/binary)
    SQLLEN      bindWidth;   // fixed-width size, or bound buffer width for var cols
    bool        bounded;     // false => long/LOB => statement takes the fallback
};
struct StmtState {
    SQLHSTMT handle;
    std::vector<BoundParam> bound;
    std::optional<std::vector<ColMeta>> cols;  // populated on first fetchBlock
    bool blockFallback = false;                 // any column unbounded
};
```

On the first `fetchBlock` after an execute, run `SQLNumResultCols` once and
`SQLDescribeCol` once per column, fill `cols`, and compute `blockFallback`.
`closeCursor` (db2_cli_driver.cpp:514) and any re-`execute` reset `cols` to
`nullopt`.

### 3.2 Bounded decision

A column is **bounded** if it is fixed-width (`SMALLINT`/`INTEGER`/`BIGINT`,
`REAL`/`FLOAT`/`DOUBLE`, `BIT`) or a character/binary type whose declared width
is `> 0` and `<= kMaxBoundColBytes` (a fixed cap, e.g. `64 * 1024`).
`DECIMAL`/`NUMERIC`/`DATE`/`TIME`/`TIMESTAMP` are carried as `SQL_C_CHAR` text
(matching `getColumn`) and are bounded because their text widths are small and
known. A column is **unbounded** if it is `CLOB`/`BLOB`/`LONG VARCHAR`/`LONG
VARGRAPHIC`, or a char/binary type with declared width `0` (unknown) or
`> kMaxBoundColBytes`.

If **all** columns are bounded → rowset path (§3.3). If **any** column is
unbounded → the whole statement uses the fallback path (§3.4). The decision is
computed once and stored in `blockFallback`.

### 3.3 Rowset path

Mirror `executeBatch` in reverse:

1. **Rows-per-block.** `R = clamp(kFetchBlockBytes / perRowWidth, 1,
   kMaxBlockRows)`, then `R = min(R, maxRows)`. `perRowWidth` is the sum of each
   column's `bindWidth`. `kFetchBlockBytes` is a fixed budget (e.g. `2 * 1024 *
   1024`); `kMaxBlockRows` caps the count for very narrow rows (e.g. `4096`). A
   single row wider than the budget still yields `R >= 1`.
2. **Bind columns.** For each column allocate an `int64`-backed buffer of
   `R × bindWidth` bytes (8-byte aligned base, exactly as the write path does at
   db2_cli_driver.cpp:316) plus an `SQLLEN[R]` indicator array. Set
   `SQL_ATTR_ROW_BIND_TYPE = SQL_BIND_BY_COLUMN`, `SQL_ATTR_ROW_ARRAY_SIZE = R`,
   `SQL_ATTR_ROW_STATUS_PTR = rowStatus[R]`, `SQL_ATTR_ROWS_FETCHED_PTR =
   &rowsFetched`. Issue one `SQLBindCol` per column with its `cType`, buffer,
   `bindWidth`, and indicator array.
3. **Fetch.** One `SQLFetch` fills up to `R` rows; read `rowsFetched`.
   `SQL_NO_DATA` with `rowsFetched == 0` → cursor exhausted (return empty block).
   `SQL_SUCCESS_WITH_INFO` is benign (`cli_ok` accepts it); per-row truncation
   should not occur because unbounded columns already forced the fallback.
4. **Transpose.** For each of the `rowsFetched` rows, for each column, read the
   indicator: `SQL_NULL_DATA` → `Value{Null{}}`; otherwise decode the slot by
   `cType` into a `Value` — `int64`/`double`/`bool` by typed copy,
   `string`/`bytes` sized by the indicator (embedded NULs preserved for binary,
   exactly as `get_binary` does today). The decode logic is shared with §3.4 so
   both paths produce identical `Value`s.
5. **Clean up.** `SQLFreeStmt(SQL_UNBIND)`, then reset `SQL_ATTR_ROW_ARRAY_SIZE =
   1` and null the status/fetched pointers, so a cached handle is clean for later
   scalar reuse (mirrors the write path's `reset_paramset`).

The buffers and indicator arrays are local to the call and outlive the
`SQLFetch`; nothing rowset-related persists on the handle after return except the
(reset) attributes.

### 3.4 Fallback path

For an unbounded statement, fetch up to `maxRows` rows row-at-a-time using the
former `fetch`/`getColumn` logic — now **private helpers** that consult the
cached `ColMeta` (§3.1) instead of calling `SQLDescribeCol` per cell. Each row is
read into a `std::vector<Value>` and appended to the block; stop at `maxRows` or
on `SQL_NO_DATA`. Identical `Value` output to §3.3, so the portable layer cannot
distinguish the paths. (This path also benefits from describe-once: the per-cell
`SQLDescribeCol` waste is gone even here.)

### 3.5 Error handling inside the driver

Any `SQLBindCol`/`SQLSetStmtAttr`/`SQLFetch`/`SQLGetData` hard failure surfaces
via `make_error(SQL_HANDLE_STMT, ...)` with SQLSTATE→`ErrorCode` classification,
exactly as the existing read methods do. On error the driver still resets the
rowset attributes/unbinds before returning so the handle is not left in a bound
state. A connection-class SQLSTATE (`08xxx`) classifies as `ErrorCode::Connection`
so the portable layer discards the connection (§4).

## 4. Portable layers

### 4.1 `ResultSet` (connection.hpp) — block buffering

`ResultSet` gains a current block, a cursor index, and an exhausted flag:

```cpp
std::vector<std::vector<detail::cli::Value>> block_;
std::size_t pos_ = 0;
bool exhausted_ = false;
```

`iterator::advance()` (today one `driver_->fetch`, connection.hpp:212) becomes:

- If `pos_ == block_.size()` and not `exhausted_`: call
  `driver_->fetchBlock(stmt_, kBlockRows)`.
  - Error → store in `error_`, poison the lease (so the cached handle is
    finalized + dropped on release), end iteration. *(unchanged semantics, now at
    block granularity.)*
  - Empty block → `exhausted_ = true`, end iteration (clean end).
  - Non-empty → swap into `block_`, reset `pos_ = 0`.
- Hand out a `Row` viewing `block_[pos_++]`.

`kBlockRows` is the portable layer's request size passed as `maxRows`; the driver
further clamps by its byte budget. The forward-only, single-active-cursor,
move-only contract and the `error()`/`ok()` accessors are unchanged. Memory grows
from one row to ≤ one block, bounded by the driver budget.

### 4.2 `Row` (connection.hpp) — view over a materialized row

`Row` stops holding a driver/handle and reading cells lazily; it views one
already-materialized `std::vector<Value>`:

```cpp
explicit Row(const std::vector<detail::cli::Value>& cells) : cells_(&cells) {}
```

`try_as<Ts...>()` keeps its arity `static_assert` and maps `(*cells_)[i]` through
`TypeBinder<F>::from_value`, returning the same `Mapping` errors on
type/null/arity mismatch. The `owner_`/`record_read_error`/`note_column_error`
plumbing (connection.hpp:92–99, 186–189, 256–258) is **removed**: a column read
can no longer fail at map time for a *driver* reason (the driver already produced
the `Value`), so a mapping failure is purely client-side and must not poison the
connection. Driver read failures are caught earlier, at `fetchBlock` (§4.1).

### 4.3 `Connection::collect<T>` (connection.hpp:409) — drain blocks

```cpp
for (;;) {
    auto blk = driver_->fetchBlock(lease.handle(), kBlockRows);
    if (!blk.ok()) { lease.poison(); return blk.error(); }
    if (blk.value().empty()) break;             // clean end
    for (auto& cells : blk.value()) {
        auto row = reflect::map_row<T>(cells);  // maps from Values
        if (!row.ok()) return row.error();      // client-side mapping error
        out.push_back(std::move(row.value()));
    }
}
```

`reflect::map_row<T>` (types.hpp) is re-pointed from `(driver, stmt, ncols)` to
`(const std::vector<Value>& cells)` — it now maps reflected fields out of an
in-hand row instead of calling `getColumn`. A mapping error returns without
poisoning (client-side); a driver/connection error already returned from
`fetchBlock` with poisoning. `run_query` (connection.hpp:449) still builds an
owning `ResultSet`; `columnCount` is still called once for the cursor.

### 4.4 Unchanged

`Database`/`ScopedTransaction` query forwarders, the async executor, named/anon
param binding, retry/reconnect classification, and the statement cache are
untouched. The facade's "discard a connection-class connection instead of
returning it poisoned" logic keys off `ErrorCode::Connection` exactly as today.

## 5. Long/LOB column behavior (per-statement fallback)

A result set is served entirely by the rowset path **or** entirely by the
fallback path, decided once at first fetch (§3.2). Rationale: array binding needs
fixed-width buffers; a `CLOB`/`BLOB`/`LONG VARCHAR`/very-wide `VARCHAR` cannot be
bound to a sane fixed width. Rather than a per-column hybrid (deferred, §1
non-goals), any unbounded column makes the whole statement fall back to the
proven row-at-a-time read — zero correctness risk, one clear switch. Bounded-only
result sets (the overwhelming common case) get the full block-fetch speedup;
result sets containing a large object still work, byte-identically, just without
the block speedup. A dedicated test (§6) proves the two paths return identical
data.

## 6. Mock + tests

- **Mock seam.** `MockCliDriver` (tests/unit/mock_cli_driver.hpp),
  `ConcurrentFakeDriver` (tests/stress/support/concurrent_fake_driver.hpp), and
  any other `ICliDriver` double drop `fetch`/`getColumn` and implement
  `fetchBlock`: serve scripted blocks (a queue of row-major `Value` blocks; an
  empty block ends the cursor) and a scriptable `Error`. Existing column-count
  scripting is reused.
- **Unit (no DB).** `test_cli_seam.cpp` moves its direct `fetch`/`getColumn`
  assertions to `fetchBlock` (block shape, empty-block end, scripted error).
  Update `test_result_set.cpp`, `test_database.cpp`, `test_reflect.cpp`:
  - rows spanning **two or more blocks** iterate/collect in order,
  - a **short final block** (< requested) followed by an empty block ends cleanly,
  - an **empty result set** yields zero rows,
  - a **mid-stream `fetchBlock` error** records `error_` and poisons the lease
    (connection-class → connection discarded by the facade),
  - a **mapping error** (NULL into non-`optional`, arity/type mismatch) returns a
    `Mapping` error and does **not** poison the connection,
  - multi-block `collect<T>` aggregates all rows,
  - NULL fidelity across a block (indicator → `Null{}`), and value fidelity for
    each `Value` alternative.
- **Integration (live, opt-in).** Append to `test_db2_roundtrip.cpp`:
  - a **multi-block** SELECT (row count > one block) with full value read-back,
  - an **all-types** round-trip through the rowset path (int/double/bool/string/
    binary/decimal/date/time/timestamp/NULL),
  - a **long-column fallback** test: a result set with a `CLOB` (or `VARCHAR`
    above the cap) returns byte-identical data to a bounded projection of the same
    rows, proving §5,
  - a **NULL-heavy** result set.
- **Stress.** The stress doubles compile against the new seam so the existing
  TSan correctness suite still builds and passes unchanged.

## 7. Performance & expectations

(Static analysis from Db2 CLI semantics + the codebase; §8 adds the empirical
check.)

- **Round-trips collapse from `R` to `⌈R / blockRows⌉`.** One `SQLFetch` per
  block instead of per row, and one `SQLBindCol` per column per block instead of
  `R·C` `SQLGetData` calls.
- **The per-cell `SQLDescribeCol` waste is eliminated entirely** — described once
  per cursor, on *both* the fast and fallback paths. This is a strict win even
  for the fallback path, independent of blocking.
- **Wider rows / more columns amplify the win**, since the eliminated per-cell
  overhead scales with `R·C`.
- **Memory is bounded** by `kFetchBlockBytes`; the streaming path pre-buffers at
  most one block (vs. one row today), a deliberate and bounded trade for the
  round-trip reduction.

## 8. Empirical validation

Add `tests/stress/perf/select_bench.cpp` — the read mirror of
`batch_insert_bench.cpp` — wired into `tests/stress/perf/CMakeLists.txt`. It
requires `HALCYON_TEST_DSN`, seeds a table with N rows of bounded columns, and
times materializing them:

- **baseline:** captured against the pre-change row-at-a-time path (recorded from
  the commit before the seam switch, or behind a build flag), and
- **block fetch:** the new path,

printing rows/sec for each. A second case times a wide-row table to show the
per-cell-overhead amplification. Run during the live-runs phase; record a results
table (date, row count, row shape, rows/sec, takeaway) in §8 "Results" below,
mirroring the array-binding spec. If block fetch is **not** faster, investigate
with superpowers:systematic-debugging before claiming done.

### Results

Measured **2026-06-29** with `tests/stress/perf/select_bench.cpp` (wired in
`tests/stress/CMakeLists.txt` — the perf sources live under `perf/` but the
targets are declared in the stress `CMakeLists.txt`, matching
`batch_insert_bench`). N = 200,000 rows materialized via `queryAs`, against the
Dockerized `icr.io/db2_community/db2:11.5.9.0` on Apple silicon under amd64
emulation:

| Row shape            | Time (200k rows) | Throughput      |
|----------------------|------------------|-----------------|
| narrow (2 cols)      | 0.090 s          | ~2,214,000 rows/s |
| wide (8 cols)        | 0.190 s          | ~1,054,000 rows/s |

**Takeaway:** block fetch sustains ~1–2M rows/s. The retired row-at-a-time path
issued, per row, one `SQLFetch` plus a `SQLDescribeCol`+`SQLGetData` for **every
cell** (≈ `rows × cols × 2` CLI calls, and a server round-trip's worth of work
per row); block fetch collapses that to one `SQLBindCol` per column per block and
one `SQLFetch` per block, and describes each column once. The saving therefore
scales with both row count and column width — visible here as the wide shape
costing ~2× the narrow shape rather than ~4× (its per-row overhead is amortized
across the block, not paid per cell). A rigorous A/B against the retired path was
not re-run as a paired binary (the old read seam no longer exists on this
branch); the figures above are the block-fetch path and the comparison is the
CLI-call-count analysis in §7.

## 9. Documentation impact

- **Parent spec** §1 non-goals: "Large-result streaming / server-side cursors"
  is partly addressed — note that forward block fetch now backs the read path
  (full scrollable cursors remain out of scope).
- `docs/guide/querying.md`: note that reads use Db2 CLI block fetch under the
  hood (no API change), and that result sets containing a LOB/long column
  transparently fall back to row-at-a-time.
- `docs/guide/advanced.md`: document the fixed internal fetch block budget and
  the bounded-column threshold as internal tunables (not public API).
- `README.md`: add block fetch to the feature list alongside array binding (the
  read/write symmetry).
