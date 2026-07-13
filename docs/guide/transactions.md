# Transactions

Halcyon provides two transaction styles. Both isolate the unit of work on a single
pooled connection; neither is auto-retried (the spec requires the caller to re-drive
the whole transaction on failure).

## Functional style — `Database::transaction`

The functional form is the most concise. Pass a lambda that receives `Transaction&`
and returns `Result<T>`:

```cpp
auto result = db.transaction([&](halcyon::Transaction& tx) -> halcyon::Result<void> {
    if (auto r = tx.execute("UPDATE accounts SET balance = balance - ? WHERE id = ?",
                            amount, from_id); !r.ok())
        return r.error();

    if (auto r = tx.execute("UPDATE accounts SET balance = balance + ? WHERE id = ?",
                            amount, to_id); !r.ok())
        return r.error();

    return {};  // signals commit
});

if (!result.ok())
    std::cerr << "transfer failed: " << result.error().message << "\n";
```

The library commits on a successful `Result` and rolls back when:
- The lambda returns an error `Result`.
- The lambda throws an exception (the exception is re-thrown after the rollback).

### Returning a value from the transaction

The lambda's `Result<T>` carries through:

```cpp
auto new_id = db.transaction(
    [&](halcyon::Transaction& tx) -> halcyon::Result<std::int64_t> {
        auto r = tx.execute("INSERT INTO orders(product_id, qty) VALUES (?, ?)",
                            product_id, qty);
        if (!r.ok()) return r.error();

        auto id_res = tx.query("SELECT IDENTITY_VAL_LOCAL() FROM SYSIBM.SYSDUMMY1");
        if (!id_res.ok()) return id_res.error();

        auto it = id_res.value().begin();
        auto [new_order_id] = it->as<std::int64_t>();
        return new_order_id;
    });

if (new_id.ok())
    std::cout << "created order " << new_id.value() << "\n";
```

### Functional style with the free function

```cpp
// Equivalent to db.transaction(fn)
halcyon::transaction(db, [](halcyon::Transaction& tx) -> halcyon::Result<void> {
    return tx.execute("DELETE FROM tmp_work WHERE done = ?", true).map([](auto) {});
});
```

## RAII style — `Database::begin`

`begin()` leases a connection and returns a `ScopedTransaction` that auto-rolls
back on destruction unless `commit()` is called:

```cpp
auto stx = db.begin();
if (!stx.ok()) {
    std::cerr << "begin failed: " << stx.error().message << "\n";
    return;
}
auto& tx = stx.value();

auto r1 = tx.execute("INSERT INTO log(event) VALUES (?)", std::string("start"));
auto r2 = tx.execute("INSERT INTO log(event) VALUES (?)", std::string("end"));

if (r1.ok() && r2.ok()) {
    auto c = tx.commit();
    if (!c.ok()) std::cerr << "commit failed: " << c.error().message << "\n";
} else {
    tx.rollback();  // explicit; also happens automatically on destruction
}
```

`ScopedTransaction` exposes the full `query` / `execute` / `queryAs` API via its
`operator->` and forwarding methods:

```cpp
auto stx = db.begin().value();  // throws if begin() failed
stx->execute("...");            // via operator->
stx.execute("...");             // forwarding overload on ScopedTransaction
stx.commit();
```

### Poisoned connections

If `commit()` or `rollback()` itself fails (network drop during flush), the
connection is marked *poisoned* and discarded rather than returned to the pool.
The pool opens a fresh replacement on the next acquire.

## Querying inside a transaction

All `query`, `execute`, and `queryAs` variants are available on `Transaction`:

```cpp
db.transaction([](halcyon::Transaction& tx) -> halcyon::Result<void> {
    auto products = tx.queryAs<Product>(
        "SELECT id, stock FROM products WHERE stock < ? FOR UPDATE", 10);
    if (!products.ok()) return products.error();

    for (const auto& p : products.value()) {
        auto r = tx.execute("UPDATE products SET stock = stock + ? WHERE id = ?",
                            100, p.id);
        if (!r.ok()) return r.error();
    }
    return {};
});
```

## Savepoints & nested transactions

A `Savepoint` marks a point inside a transaction you can roll back to without
abandoning the whole unit of work. Names are auto-generated
(`halcyon_sp_1`, ...) or explicit (validated: `[A-Za-z_][A-Za-z0-9_]{0,127}`,
not starting with `SYS`).

```cpp
auto tx = conn.begin().value();
tx.execute("INSERT INTO orders VALUES (?)", 1).value();

auto sp = tx.savepoint().value();           // or tx.savepoint("stage1")
tx.execute("INSERT INTO orders VALUES (?)", 2).value();
sp.rollback().value();                       // undoes only the second insert
tx.commit().value();                         // first insert persists
```

Like `Transaction`, a `Savepoint` is a one-shot RAII guard: `release()` keeps
the work, `rollback()` undoes it, and a destructor that saw neither rolls
back and releases (undo by default). A failed savepoint statement poisons the
savepoint *and* its transaction, so the pooled connection is discarded rather
than reused. Destroy savepoints before their transaction ends or moves, inner
before outer.

`Transaction::nested(fn)` wraps `fn` in a savepoint scope — the functional
style for "try this part, keep the rest":

```cpp
auto r = tx.nested([&](halcyon::Transaction& t) -> halcyon::Result<std::int64_t> {
    return t.execute("INSERT INTO audit VALUES (?)", id);
});
// ok result  -> savepoint released (work kept)
// error/throw -> rolled back to the savepoint; outer transaction continues
```

Calling `commit()`/`rollback()` on the transaction inside `nested()` is a
programming error and yields `ErrorCode::InvalidState`.

## Isolation levels

Halcyon uses Db2's own terminology — `UncommittedRead` (UR),
`CursorStability` (CS, the Db2 default), `ReadStability` (RS),
`RepeatableRead` (RR) — because the CLI constants behind them do **not** line
up with the SQL-standard names (Db2 RR maps to the CLI's SERIALIZABLE).

Set a pool-wide session default, a per-transaction override, or both:

```cpp
halcyon::PoolConfig cfg;
cfg.isolation = halcyon::Isolation::CursorStability;   // every connection

auto tx = conn.begin(halcyon::Isolation::RepeatableRead).value();
// ... transaction runs at RR ...
tx.commit().value();   // connection restored to its default level

db.transaction(halcyon::Isolation::UncommittedRead, [](auto& tx) {
    return tx.template queryAs<Row>("SELECT ...");
});
```

The override is restored on **every** exit path (commit, rollback,
destructor). If the restore fails the transaction is poisoned and the
connection discarded — a pooled connection never carries a surprise
isolation level.

## Do not auto-retry transactions

Halcyon's retry logic is disabled for `executeBatch` and is intentionally absent
for transactions. Replaying a multi-step transaction blindly is unsafe because
side-effects from the first attempt may persist (e.g. incrementing a counter).
If your transaction is genuinely idempotent, implement the retry loop yourself
around the whole `db.transaction(...)` call.
