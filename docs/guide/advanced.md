# Advanced

## Statement cache

Each connection maintains a per-connection LRU cache of prepared statement
handles. Reusing a handle skips the `SQLPrepare` round-trip on subsequent calls
with the same SQL string.

### Configuration

```cpp
halcyon::PoolConfig config{
    .statementCacheSize = 64,   // default; 0 disables the cache entirely
};
```

`statementCacheSize` is the maximum number of prepared statements held per
connection. The LRU evicts the least-recently-used entry when full.

### Metrics

The statement cache emits two metrics when observability is configured:

| Metric | `result` label | When emitted |
|---|---|---|
| `halcyon_stmt_cache_total` | `hit` | Statement handle reused from cache |
| `halcyon_stmt_cache_total` | `miss` | New `SQLPrepare` call (cold or after eviction) |
| `halcyon_stmt_cache_total` | `evict` | An existing entry was evicted to make room |
| `halcyon_stmt_cache_total` | `overflow` | Cache disabled (`size=0`) or size not set |
| `halcyon_stmt_cache_size` | — (gauge) | Current count of cached handles per connection |

Monitor the hit rate — a high miss rate for frequently-repeated SQL may indicate
the cache is too small for your workload:

```
hit_rate = halcyon_stmt_cache_total{result="hit"} /
           (halcyon_stmt_cache_total{result="hit"} + halcyon_stmt_cache_total{result="miss"})
```

### When to tune the cache size

- Increase if you have many distinct prepared statements and see frequent `evict`
  events.
- Decrease or disable (`= 0`) for workloads that are almost entirely unique
  ad-hoc SQL (no repeated queries); the overhead of checking the cache is then
  not worth it.
- `statementCacheSize = 0` forces a fresh `SQLPrepare` and `SQLFreeStmt` on every
  call — equivalent to Halcyon's behaviour before the cache was added.

---

## Custom `TypeBinder<T>` { #custom-typebinder }

Specialize `TypeBinder<T>` to bind and read any type that Halcyon does not know
about. Place the specialization in your own header (not in Halcyon source) after
`#include <halcyon/types.hpp>`.

### `to_value` — bind a C++ value as a query parameter

```cpp
static halcyon::detail::cli::Value to_value(const MyType& v);
```

`Value` is `std::variant<Null, bool, int64_t, double, std::string, std::vector<std::byte>>`.
Map your type to the closest SQL-compatible alternative.

### `from_value` — read a column into a C++ value

```cpp
static halcyon::Result<MyType> from_value(const halcyon::detail::cli::Value& v);
```

Return a `Mapping` error for unexpected alternatives or NULL:

```cpp
using halcyon::detail::cli::Null;

// Example: a UUID stored as CHAR(36)
struct UUID { std::string value; };

template <>
struct halcyon::TypeBinder<UUID> {
    static halcyon::detail::cli::Value to_value(const UUID& u) {
        return halcyon::detail::cli::Value{u.value};
    }
    static halcyon::Result<UUID> from_value(const halcyon::detail::cli::Value& v) {
        if (std::holds_alternative<Null>(v))
            return halcyon::Error{halcyon::ErrorCode::Mapping, {}, 0,
                                  "NULL into non-optional UUID"};
        if (auto* s = std::get_if<std::string>(&v))
            return UUID{*s};
        return halcyon::Error{halcyon::ErrorCode::Mapping, {}, 0,
                              "UUID: expected string column"};
    }
};
```

After this specialization, `UUID` works in all parameter and result contexts:

```cpp
UUID id{"550e8400-e29b-41d4-a716-446655440000"};
db.execute("INSERT INTO items(uuid, name) VALUES (?, ?)", id, std::string("widget"));

auto r = db.queryAs<Item>("SELECT uuid, name FROM items WHERE uuid = ?", id);
```

### Nullability

To make your type nullable, also specialize `TypeBinder<std::optional<MyType>>`:

```cpp
template <>
struct halcyon::TypeBinder<std::optional<UUID>> {
    static halcyon::detail::cli::Value to_value(const std::optional<UUID>& o) {
        if (o) return halcyon::detail::cli::Value{o->value};
        return halcyon::detail::cli::Value{halcyon::detail::cli::Null{}};
    }
    static halcyon::Result<std::optional<UUID>> from_value(
        const halcyon::detail::cli::Value& v) {
        if (std::holds_alternative<Null>(v)) return std::optional<UUID>{};
        if (auto* s = std::get_if<std::string>(&v)) return std::optional<UUID>{UUID{*s}};
        return halcyon::Error{halcyon::ErrorCode::Mapping, {}, 0, "UUID: expected string"};
    }
};
```

The built-in `TypeBinder<std::optional<U>>` already handles this pattern for
built-in types — you only need to specialize it for your own custom types.

---

## Driver injection for unit tests

`Database::open` has an overload that takes a reference to `ICliDriver`, letting
you swap in `MockCliDriver` for tests that do not need a real database:

```cpp
#include <halcyon/halcyon.hpp>
#include "tests/unit/mock_cli_driver.hpp"  // or your own mock

halcyon::MockCliDriver mock;
auto db = halcyon::Database::openOrThrow(mock, "DATABASE=FAKE;");

// Program mock responses
mock.pushResult(...);

// Exercise your application code against the mock
auto r = db.queryAs<Product>("SELECT id, name FROM products");
```

**Lifetime rule for the raw-reference overload:** `mock` must remain alive for the
entire lifetime of `db` *and* every `QueryResult`, `ScopedTransaction`, and async
future derived from `db`. If any of those can outlive `mock`, use the
`shared_ptr<ICliDriver>` overload instead:

```cpp
auto mock = std::make_shared<halcyon::MockCliDriver>();
auto db   = halcyon::Database::openOrThrow(mock, "DATABASE=FAKE;");
// mock is co-owned by db and any QueryResult/ScopedTransaction derived from db.
```

---

## `with_retry` — custom retry loops

Use `with_retry` directly when you need retry logic outside the pool (e.g., a
one-off operation with no pool):

```cpp
#include <halcyon/retry.hpp>

halcyon::ExecPolicy policy = halcyon::ExecPolicy::idempotent(5);
policy.backoff.baseDelay = std::chrono::milliseconds(200);
policy.backoff.maxDelay  = std::chrono::milliseconds(10000);

auto r = halcyon::with_retry(policy, [&] {
    return db.execute("CALL myproc(?)", arg);
});
```

`with_retry` calls `fn()` and retries while `r.error().retriable` is true and
attempts remain. It does not consult any pool — it is a thin loop over `fn`.

---

## `is_read_only` classification

The library uses `detail::is_read_only(sql)` internally to decide whether a
`query` call is safe to auto-retry. It recognises:

- **Read-only:** `SELECT`, `VALUES`, `WITH … SELECT` (CTE whose main body is a SELECT)
- **Not read-only:** `INSERT`, `UPDATE`, `DELETE`, `MERGE` at any nesting depth;
  `WITH … INSERT/UPDATE/DELETE/MERGE`; `SELECT … FROM FINAL TABLE(INSERT …)`

The check is token-level and literal-aware (string literals and delimited identifiers
are skipped), so column values containing DML keywords are never misclassified.
You can call it yourself when building custom retry policies:

```cpp
bool safe = halcyon::detail::is_read_only("SELECT id FROM users WHERE id = ?");
// true
bool safe2 = halcyon::detail::is_read_only("WITH w AS (SELECT 1) UPDATE t SET x=1");
// false
```

## Fetch block tuning (internal)

Reads use Db2 CLI rowset (block) fetch under the hood. The driver sizes each block
from a fixed internal byte budget (~2 MiB) and a row cap, and treats any column
wider than a fixed threshold (~64 KiB) — or a `CLOB`/`BLOB`/`LONG VARCHAR` — as
"long", routing that statement to a row-at-a-time fallback that still reads the
data correctly. These are internal constants, not public API; they mirror the
write path's array-binding byte budget. There is intentionally no knob to tune them
in this release.
