# Querying

## `query` — streaming cursor

`Database::query` opens a forward-only cursor. The returned `QueryResult` owns both
the cursor and the pooled connection lease for its lifetime. Rows are read from the
server in blocks (see [How rows arrive](#how-rows-arrive-block-fetch)) and surfaced
to you one row at a time.

```cpp
auto res = db.queryOrThrow(
    "SELECT id, name, score FROM leaderboard ORDER BY score DESC");

for (auto& row : res) {
    auto [id, name, score] = row.as<int, std::string, double>();
    std::cout << id << " " << name << " " << score << "\n";
}
```

## How rows arrive (block fetch)

Reads use Db2 CLI **rowset (block) fetch**: columns are bound once and the driver
fills a block of rows per round-trip, so `query`/`queryAs` collapse what would be
per-row, per-column CLI calls into a handful of fetches. This is transparent — the
API is unchanged. A result set that contains a large object (`CLOB`/`BLOB`/`LONG
VARCHAR`/very wide `VARCHAR`) transparently falls back to a row-at-a-time read for
correctness; everything else uses the fast path.

Because a streaming `QueryResult` keeps its cursor open, it holds a read lock on the
underlying table — most visibly for LOB columns, where the cursor stays open until
you finish. Destroy the `QueryResult` (let it leave scope) before issuing DDL such
as `DROP`/`ALTER` on the same table, or the DDL will block on the cursor's lock.

## `query` — lifetime

**Lifetime rule:** the pooled connection is held until `QueryResult` is destroyed.
Release it promptly — store results in a `std::vector` if you need them long-term.

```cpp
std::vector<std::tuple<int, std::string>> users;
{
    auto res = db.queryOrThrow("SELECT id, name FROM users WHERE active = ?", true);
    for (auto& row : res)
        users.push_back(row.as<int, std::string>());
}  // connection released here
```

### Checking for mid-stream errors

A connection drop during a long streaming query is reported after the loop:

```cpp
auto res = db.queryOrThrow("SELECT * FROM events");
for (auto& row : res) { /* ... */ }
if (!res.ok())
    std::cerr << "stream error: " << res.error()->message << "\n";
```

## `queryAs<T>` — materialized struct mapping

`queryAs` materializes all rows into a `std::vector<T>` before returning. It
requires `HALCYON_REFLECT(T, ...)` to map columns to struct fields by position.

```cpp
struct Product {
    int         id;
    std::string name;
    double      price;
    int         stock;
};
HALCYON_REFLECT(Product, id, name, price, stock);

// Returns Result<std::vector<Product>>
auto products = db.queryAsOrThrow<Product>(
    "SELECT id, name, price, stock FROM products WHERE category = ?",
    std::string("electronics"));

for (const auto& p : products)
    std::cout << p.name << " $" << p.price << "\n";
```

Columns are matched **by position** (left-to-right) — the field order in
`HALCYON_REFLECT` must match the `SELECT` column order. See
[Parameters & Types](parameters-and-types.md#halcyon_reflect) for details.

## `Row::as<Ts...>` — tuple extraction

Inside a streaming loop, `row.as<Ts...>()` extracts the current row into a tuple:

```cpp
// Throws halcyon::MappingException on column-count mismatch or type error.
auto [id, name] = row.as<int, std::string>();

// Result<> form — no exceptions.
auto r = row.try_as<int, std::string>();
if (!r.ok()) { /* handle mapping error */ }
auto [id, name] = r.value();
```

Type errors raise `ErrorCode::Mapping` (or `MappingException` in the throwing
form). See [Parameters & Types](parameters-and-types.md) for the full type table.

## `execute` — DML / DDL

Use `execute` for statements that return a row count, not rows:

```cpp
std::int64_t n = db.executeOrThrow(
    "DELETE FROM sessions WHERE expires_at < ?",
    halcyon::Timestamp{"2026-01-01 00:00:00"});
std::cout << n << " expired sessions removed\n";
```

`execute` returns `Result<std::int64_t>` (the affected-row count); `executeOrThrow`
unwraps it.

## Named parameters

Both `query` and `execute` accept a `halcyon::params` object as the last argument
for named placeholder style:

```cpp
auto res = db.queryOrThrow(
    "SELECT * FROM orders WHERE customer_id = :cid AND status = :status",
    halcyon::params{{"cid", 101}, {"status", std::string("shipped")}});
```

Named placeholders are rewritten to positional `?` before the statement is
prepared. A `:name` inside a string literal, delimited identifier, or comment is
never substituted. See [Parameters & Types](parameters-and-types.md#named-parameters)
for full details.

## Functional free-function API

`halcyon::query`, `halcyon::execute`, `halcyon::query_as`, and
`halcyon::transaction` are free functions that delegate to `Database` members.
They are useful in generic code and pipelines where passing `Database&` directly
is awkward:

```cpp
auto res  = halcyon::query(db, "SELECT 1 FROM SYSIBM.SYSDUMMY1");
auto rows = halcyon::query_as<Product>(db, "SELECT id, name, price, stock FROM products");
auto n    = halcyon::execute(db, "DELETE FROM tmp_log WHERE ts < ?",
                             halcyon::Timestamp{"2026-01-01 00:00:00"});
```

## Column count

`QueryResult::column_count()` reports the number of columns in the result set:

```cpp
auto res = db.queryOrThrow("SELECT * FROM some_view");
std::cout << res.column_count() << " columns\n";
```
