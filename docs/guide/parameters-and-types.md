# Parameters & Types

## Anonymous parameters (`?`)

Pass positional values as variadic arguments after the SQL string:

```cpp
// One placeholder per argument, in order.
db.execute("UPDATE users SET name = ?, email = ? WHERE id = ?",
           std::string("Alice"), std::string("alice@example.com"), 42);
```

String literals are converted to `std::string` automatically:

```cpp
db.query("SELECT * FROM users WHERE city = ?", "NYC");  // works
```

## Named parameters (`:name`) { #named-parameters }

Build a `halcyon::params` object and pass it as the last argument:

```cpp
db.execute(
    "UPDATE users SET name = :name, email = :email WHERE id = :id",
    halcyon::params{
        {"name",  std::string("Alice")},
        {"email", std::string("alice@example.com")},
        {"id",    42}
    });
```

Rules:
- A placeholder is `:[a-zA-Z_][a-zA-Z0-9_]*`.
- A repeated name (`:id` twice) re-binds the same value.
- `::` is passed through literally (Db2 scope-qualifier style).
- Placeholders inside `'string literals'`, `"delimited identifiers"`,
  `-- line comments`, and `/* block comments */` are never substituted.

## Built-in type mapping

The table below covers what `TypeBinder<T>` supports out of the box.
`Result<T>` binding and reading `std::optional<T>` works for all rows with nullable columns.

| C++ type | SQL / Db2 type | Notes |
|---|---|---|
| `bool` | SMALLINT (0/1) | Reads from SMALLINT or BOOLEAN |
| `int`, `short`, `int64_t`, … | INTEGER / BIGINT / SMALLINT | Range-checked on read |
| `float`, `double` | REAL / DOUBLE | `float` widens to `double` at the seam |
| `std::string` | VARCHAR / CHAR / CLOB | UTF-8 |
| `std::string_view` | VARCHAR | Bind-only; use `std::string` to read |
| `std::vector<std::byte>` | BLOB / BINARY | |
| `std::optional<T>` | any nullable column | Binds NULL when empty; reads as empty on NULL |
| `halcyon::Decimal` | DECIMAL / NUMERIC | Exact text round-trip (e.g. `"123.4500"`) |
| `halcyon::Date` | DATE | Text round-trip, e.g. `"2026-06-22"` |
| `halcyon::Time` | TIME | Text round-trip, e.g. `"13:45:00"` |
| `halcyon::Timestamp` | TIMESTAMP | Text round-trip, e.g. `"2026-06-22 13:45:00.123456"` |
| `std::chrono::system_clock::time_point` | TIMESTAMP | UTC ISO-8601 text via SQL_C_CHAR |

### Nullable columns

Wrap any type in `std::optional<T>` to handle SQL NULL without errors:

```cpp
// Column "phone" may be NULL.
auto res = db.queryOrThrow("SELECT name, phone FROM contacts WHERE id = ?", 7);
for (auto& row : res) {
    auto [name, phone] = row.as<std::string, std::optional<std::string>>();
    if (phone)
        std::cout << name << ": " << *phone << "\n";
    else
        std::cout << name << ": no phone\n";
}
```

### Decimal and date/time types

```cpp
// Exact decimal
db.execute("INSERT INTO prices(item, amount) VALUES (?, ?)",
           std::string("widget"), halcyon::Decimal{"9.99"});

auto res = db.queryOrThrow("SELECT amount FROM prices WHERE item = ?",
                           std::string("widget"));
for (auto& row : res) {
    auto [d] = row.as<halcyon::Decimal>();
    std::cout << d.str() << "\n";  // "9.99"
}

// chrono interop
auto now = std::chrono::system_clock::now();
db.execute("INSERT INTO events(ts) VALUES (?)", now);

auto rows = db.queryOrThrow("SELECT ts FROM events");
for (auto& row : rows) {
    auto [tp] = row.as<std::chrono::system_clock::time_point>();
    // tp is a system_clock::time_point in UTC
}
```

## `HALCYON_REFLECT` — struct mapping { #halcyon_reflect }

Place `HALCYON_REFLECT` in the **global namespace** after the struct definition:

```cpp
struct User {
    int64_t     id;
    std::string name;
    std::string email;
    bool        active;
};
HALCYON_REFLECT(User, id, name, email, active);
```

Then use `queryAs<User>`:

```cpp
auto users = db.queryAsOrThrow<User>(
    "SELECT id, name, email, active FROM users WHERE active = ?", true);
```

**Column matching is positional.** The order of fields in `HALCYON_REFLECT` must
match the column order in the `SELECT` list. Aliasing columns in the SQL is the
easiest way to control order:

```cpp
// Reorder by giving aliases that match nothing — just control position.
auto r = db.queryAsOrThrow<User>(
    "SELECT user_id AS id, full_name AS name, email_addr AS email, is_active AS active "
    "FROM accounts WHERE dept_id = ?", 5);
```

`HALCYON_REFLECT` supports up to 10 fields. Extend the internal
`HALCYON_FE_<n>` ladder in `types.hpp` for more.

## `batchOf` — structs to batch rows

For bulk inserts, `batchOf` converts a `std::vector<T>` of reflected structs
into a `Batch` where each field becomes a positional bind:

```cpp
struct LogEntry {
    int64_t     ts_ms;
    std::string level;
    std::string message;
};
HALCYON_REFLECT(LogEntry, ts_ms, level, message);

std::vector<LogEntry> entries = /* ... */;
db.executeBatch("INSERT INTO app_log(ts_ms, level, message) VALUES (?,?,?)",
                halcyon::batchOf(entries));
```

See [Batch Operations](batch-operations.md) for more.

## Custom `TypeBinder<T>`

To bind a type Halcyon does not know about, specialize `TypeBinder<T>`:

```cpp
#include <halcyon/types.hpp>

// Bind a custom UUID type as a 36-char string.
struct UUID { std::string str; };

template <>
struct halcyon::TypeBinder<UUID> {
    static halcyon::detail::cli::Value to_value(const UUID& u) {
        return halcyon::detail::cli::Value{u.str};
    }
    static halcyon::Result<UUID> from_value(const halcyon::detail::cli::Value& v) {
        if (std::holds_alternative<halcyon::detail::cli::Null>(v))
            return halcyon::Error{halcyon::ErrorCode::Mapping, {}, 0, "NULL UUID"};
        if (auto* s = std::get_if<std::string>(&v)) return UUID{*s};
        return halcyon::Error{halcyon::ErrorCode::Mapping, {}, 0, "UUID: expected string"};
    }
};
```

After specializing, `UUID` can be used anywhere Halcyon accepts a bindable type.
See [Advanced](advanced.md#custom-typebinder) for more details.
