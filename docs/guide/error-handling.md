# Error Handling

Halcyon exposes every operation in two styles. Choose per call site — they can be
mixed freely in the same program.

## `Result<T>` — no-exception style

```cpp
halcyon::Result<halcyon::QueryResult> r = db.query(
    "SELECT id, name FROM users WHERE active = ?", true);

if (!r.ok()) {
    const halcyon::Error& e = r.error();
    std::cerr << "query failed: " << e.message
              << " (SQLSTATE " << e.sqlstate << ")\n";
    return;
}

for (auto& row : r.value()) {
    auto [id, name] = row.as<int, std::string>();
}
```

`Result<T>` is a lightweight wrapper around `std::variant<T, Error>`:

| Member | Returns |
|---|---|
| `r.ok()` | `true` if the value is held |
| `r.error()` | The `Error` (UB to call when `ok()`) |
| `r.value()` | The value (throws on error — see below) |
| `r.value_or(fallback)` | The value, or `fallback` on error |
| `r.map(f)` | Transforms the value; propagates errors |
| `r.and_then(f)` | Chains a fallible step; propagates errors |

`r.value()` is the bridge to exception land: calling it on a failed `Result` throws the appropriate `halcyon::Exception` subclass. This makes it safe to use `r.value()` in code that uses exceptions for control flow:

```cpp
// This throws halcyon::ConnectionException if open fails.
auto db = halcyon::Database::open(dsn).value();
```

## Throwing style

Every method has an `OrThrow` counterpart that unwraps `Result::value()`:

| Result style | Throwing style |
|---|---|
| `Database::open(dsn)` | `Database::openOrThrow(dsn)` |
| `db.query(sql, ...)` | `db.queryOrThrow(sql, ...)` |
| `db.execute(sql, ...)` | `db.executeOrThrow(sql, ...)` |
| `db.queryAs<T>(sql, ...)` | `db.queryAsOrThrow<T>(sql, ...)` |

## `Error` structure

```cpp
struct Error {
    ErrorCode   code;         // semantic category (see below)
    std::string sqlstate;     // 5-char SQLSTATE, e.g. "08001"
    int         nativeError;  // Db2 SQLCODE / native error code
    std::string message;      // text from SQLGetDiagRec
    bool        retriable;    // true → the library's auto-retry may replay this
};
```

### `ErrorCode` values

| Code | When raised |
|---|---|
| `Connection` | Network drop, DSN unreachable, authentication failure |
| `Timeout` | Pool acquire timed out, or statement execution exceeded a limit |
| `Constraint` | Unique/FK/check constraint violation |
| `Syntax` | SQL parse error (SQLSTATE `42…`) |
| `Deadlock` | Db2 deadlock victim (SQLSTATE `40001`) |
| `Transient` | Transient server-side error; may succeed on retry |
| `Mapping` | C++ ↔ SQL type mismatch or NULL into a non-optional field |
| `Pool` | Pool limit reached and acquire timed out |
| `Unknown` | Any error not matched by the above categories |

`retriable` is set automatically based on SQLSTATE and is used by the library's
[auto-retry](connection-pool.md#auto-retry) logic. You do not normally inspect it
yourself unless you implement a custom retry loop with `with_retry`.

## Exception class hierarchy

When errors are surfaced via exceptions (throwing overloads or `result.value()` on
a failed result), Halcyon throws a specific subtype:

```
halcyon::Exception  (: std::runtime_error)
  ├─ halcyon::ConnectionException
  ├─ halcyon::QueryException
  ├─ halcyon::ConstraintException
  ├─ halcyon::TimeoutException
  ├─ halcyon::TransientException
  └─ halcyon::MappingException
```

All subtypes carry the full `Error` struct:

```cpp
try {
    db.executeOrThrow("INSERT INTO users(email) VALUES (?)", "dup@example.com");
} catch (const halcyon::ConstraintException& e) {
    std::cerr << "duplicate: " << e.error().message << "\n";
} catch (const halcyon::Exception& e) {
    std::cerr << "db error: " << e.what() << "\n";
}
```

## Chaining with `and_then` and `map`

```cpp
auto count = db.query("SELECT COUNT(*) FROM users")
    .and_then([](halcyon::QueryResult qr) -> halcyon::Result<std::int64_t> {
        auto it = qr.begin();
        if (it == qr.end()) return halcyon::Error{halcyon::ErrorCode::Unknown, {}, 0, "no rows"};
        auto [n] = it->as<std::int64_t>();
        return n;
    });

if (count.ok())
    std::cout << count.value() << " users\n";
```

## Checking `QueryResult` after iteration

`query()` opens a streaming cursor; fetch errors mid-iteration are recorded on the
result. Check `ok()` and `error()` after the loop:

```cpp
auto res = db.queryOrThrow("SELECT * FROM large_table");
for (auto& row : res) { /* process */ }

if (!res.ok()) {
    std::cerr << "fetch error: " << res.error()->message << "\n";
}
```
