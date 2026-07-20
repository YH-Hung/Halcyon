# Coroutines (C++20)

`halcyon/coro.hpp` adds awaitable forms of every async-capable operation —
`execute`, `query`, `executeBatch`, `transaction`, and streaming — for C++20
consumers. It is **opt-in**: the compiled library stays C++17, the header is
not part of the `halcyon.hpp` umbrella, and including it from a TU without
C++20 coroutine support is a clear `#error`. Toolchain floor: GCC 11+,
Clang 14+, AppleClang 14+.

```cpp
#include <halcyon/coro.hpp>   // requires -std=c++20 in THIS TU

halcyon::coro::Task<halcyon::Result<int>> flow(halcyon::Database db) {
    auto n = co_await halcyon::coro::execute(
        db, "UPDATE orders SET status=? WHERE id=?", std::string{"SHIPPED"}, 42);
    if (!n.ok()) co_return n.error();
    auto rows = co_await halcyon::coro::query<Order>(
        db, "SELECT id, status FROM orders WHERE status=?", std::string{"NEW"});
    if (!rows.ok()) co_return rows.error();
    co_return static_cast<int>(rows.value().size());
}

int main() {
    auto db = halcyon::Database::openOrThrow(dsn);
    auto r = halcyon::coro::syncWait(flow(db));   // bridge from sync code
}
```

## Execution model

Each awaited operation offloads the existing blocking call to the same
executor thread pool that backs `executeAsync`, and the coroutine resumes on
the completing worker. Everything the sync API guarantees is inherited
unchanged — auto-retry of idempotent statements, transparent reconnect,
metrics, tracing (the OTel context is captured where you *call* the factory,
exactly like `executeAsync`), and logging.

Coroutines change the ergonomics, not the concurrency limits: the CLI
underneath is blocking, so throughput is still bounded by executor threads ×
blocking calls.

## `Task<T>` semantics

- **Lazy** — nothing runs until the task is `co_await`ed (or passed to
  `syncWait`). A task that is never awaited never runs and destroys cleanly.
- **Move-only, single-consumer** — await a given task at most once.
- **Exceptions propagate out of `co_await`** — the throwing style is
  `(co_await halcyon::coro::query<T>(db, sql)).value()`. There is no doubled
  overload set: every operation returns `Task<Result<...>>`.
- Deep task chains complete via symmetric transfer without growing the stack.

## Threading contract

- **Continuations resume on Halcyon executor worker threads.** Don't run
  long CPU-bound work there; hop to your own scheduler for that.
- **Never call `coro::syncWait` from an executor worker** (i.e. from inside
  any continuation) — the awaited task needs a worker, so blocking one can
  self-deadlock. Debug builds assert via `Executor::onWorkerThread()`.
- One connection/transaction/stream is used by **at most one thread at a
  time** across awaits. Sequential use from different worker threads is
  within the single-owner contract: IBM permits a CLI handle to move between
  threads when access is coordinated and never concurrent, and awaits
  sequence all access with happens-before edges through the executor.

## Lifetime rules

- Arguments are **owned by the coroutine** before it can suspend: factories
  copy `sql` and materialize every ordinary parameter (including
  `string_view`/`const char*`) into owning storage at the call site. Passing
  temporaries is safe by construction.
- `LobSource` **wrappers** are owned by the frame too — passing
  `halcyon::lobFile("report.pdf")` inline is the expected idiom. Only a
  wrapper's *referents* are borrowed: `lobStream`'s stream and anything a
  `lobCallback` captures by reference must outlive the returned `Task`.
- A coroutine frame holds the pool (and the owned driver) alive, but only a
  **weak** reference to the executor. An in-flight task drains safely if
  every `Database` copy is destroyed; a not-yet-awaited task completes its
  await with `ErrorCode::InvalidState` instead of undefined behavior. A lazy
  task can outlive its backing only until awaited.

## Transactions

```cpp
auto r = co_await halcyon::coro::transaction(db,
    [](halcyon::Transaction& tx) -> halcyon::Result<int> {
        auto e = tx.execute("INSERT INTO audit VALUES (?)", 1);
        if (!e.ok()) return e.error();
        return 1;
    });
```

- A **plain callable** returning `Result<T>` runs entirely on one worker.
  Use the normal blocking `tx.execute(...)`/`tx.query(...)` inside — per-op
  awaitables there would be pointless executor hops.
- A **`Task`-returning coroutine** may await non-Halcyon awaitables
  mid-transaction. A foreign awaitable controls where it resumes, so after
  it every `tx.*` call blocks whatever thread `fn` currently occupies. The
  scope itself hops back to a Halcyon worker before commit/rollback. If the
  executor is already gone at that point, the scope rolls back synchronously
  on the current thread rather than leak an open transaction.
- Terminal semantics are the sync `transaction(...)`'s: ok → commit; error
  or exception → rollback then propagate; restore/rollback failures poison
  so the pool discards the connection.
- Tracing: a plain callable gets the usual `halcyon.transaction` span. A
  `Task`-returning `fn` does **not** — that path cannot run the instrumented
  sync scope across foreign-thread hops, so the span is deliberately omitted
  rather than emitted with wrong nesting. Trace-context propagation and the
  other spans are unaffected.
- Keep transactional scopes short — the pooled connection is held across
  every await inside the scope.

## Async streaming

```cpp
auto sq = (co_await halcyon::coro::queryStreaming(
               db, "SELECT id, doc FROM docs")).value();
while (auto row = co_await sq.next()) {
    auto id = row->get<std::int64_t>(0).value();   // scalar: synchronous
    auto reader = row->lob(1).value();             // LOB: awaitable chunks
    std::byte buf[64 * 1024];
    for (;;) {
        auto n = (co_await reader.read(buf, sizeof buf)).value();
        if (n == 0) break;   // EOF (check reader.isNull() for SQL NULL)
        consume(buf, n);
    }
}
if (!sq.ok()) handle(*sq.error());   // nullopt means end OR error
```

- `next()` offloads the row fetch; scalar `get<T>` reads client-side data
  already fetched with the row (no hop). Drain helpers `toVector()`,
  `toString()`, `toFile(path)` loop once on a worker — a single offload for
  the whole drain.
- Ascending-column-order, NULL-LOB semantics, and mid-stream abandonment
  behavior are the v1.1 streaming rules, unchanged.
- **Borrow rules:** `sq.next()`, `reader.read(...)`, and the drain helpers
  borrow their receiver (and `read` borrows your buffer) from the start of
  the await until it completes — await immediately; storing a member
  awaitable for later is unsupported. A `StreamingRow` borrows its
  `StreamingQuery` and is invalidated by the next `next()`.

## Pool health

`Database::poolStats()` returns a consistent snapshot of pool counters —
see [Connection Pool](connection-pool.md#pool-health-stats).
