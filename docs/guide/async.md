# Async

`Database::executeAsync` and `Database::queryAsync` dispatch work to a shared
thread pool and return `std::future`. The pool size equals `max` from `PoolConfig`
(minimum 1 thread).

## `executeAsync` — fire-and-forget DML

```cpp
// Returns std::future<Result<std::int64_t>>
auto f = db.executeAsync(
    "INSERT INTO audit_log(event, user_id, ts) VALUES (?, ?, ?)",
    std::string("login"), user_id, std::chrono::system_clock::now());

// Do other work concurrently, then collect the result.
auto r = f.get();
if (!r.ok())
    std::cerr << "async write failed: " << r.error().message << "\n";
```

## `queryAsync<T>` — parallel reads

`queryAsync` materialises rows into `std::vector<T>` — the future carries the
full result set, not a live cursor. This is intentional: a live cursor cannot be
passed across thread boundaries safely.

```cpp
// Returns std::future<Result<std::vector<Product>>>
auto fut = db.queryAsync<Product>(
    "SELECT id, name, price, stock FROM products WHERE category = ?",
    std::string("electronics"));

// Meanwhile, issue other work.
auto stats_fut = db.executeAsync("UPDATE category_stats SET last_queried = CURRENT_TIMESTAMP "
                                 "WHERE name = ?", std::string("electronics"));

// Collect results.
auto products = fut.get();
if (products.ok())
    for (const auto& p : products.value()) { /* ... */ }
```

## Parallel fan-out

The executor is shared across all copies of `Database`, so multiple concurrent
futures draw from the same worker pool and connection pool:

```cpp
// Issue N parallel queries.
std::vector<std::future<halcyon::Result<std::vector<Order>>>> futures;
for (int region = 1; region <= 8; ++region) {
    futures.push_back(db.queryAsync<Order>(
        "SELECT id, total FROM orders WHERE region_id = ?", region));
}

for (auto& f : futures) {
    auto result = f.get();
    if (result.ok())
        for (const auto& o : result.value()) process(o);
}
```

## Lifetime rules

`Database` is a copyable handle wrapping `shared_ptr`s. The async tasks capture
the shared pool (not `this`), so destroying the `Database` copy that launched a
task does not cancel or invalidate it — as long as any copy is alive, the shared
executor drains in-flight tasks before the backing is torn down.

```cpp
{
    halcyon::Database db2 = db;          // copy shares the same pool
    auto f = db2.executeAsync("...");
}  // db2 is gone, but the task still runs and db still holds the pool alive.
auto r = f.get();                       // safe
```

The executor's destructor joins all worker threads, so going out of scope (even
before `f.get()`) is safe — it will block until all tasks complete.

## Error handling

Async futures carry the same `Result<T>` as the synchronous overloads:

```cpp
auto f = db.queryAsync<User>("SELECT id, name FROM users WHERE id = ?", 999);
auto r = f.get();
if (!r.ok()) {
    std::cerr << "error: " << r.error().message
              << " (" << halcyon::to_string(r.error().code) << ")\n";
}
```

## No async streaming

There is no `std::future<QueryResult>` — a streaming cursor tied to a pooled
connection cannot be safely moved across thread boundaries. Use `queryAsync<T>`
(materialised) for async reads. If you need async streaming, drain the cursor
inside the async task and return a `std::vector`.

## Coroutine readiness

The executor's `submit()` method is a standard thread-pool chokepoint compatible
with a future C++20 coroutine adapter. The current `std::future` API can be
wrapped by a coroutine adapter without changing any internals.
