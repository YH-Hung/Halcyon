# Batch Operations

`Database::executeBatch` inserts multiple rows using a single prepared statement,
calling execute once per row and accumulating the total affected-row count.

## `batchOf` from reflected structs

The most common pattern: a `std::vector<T>` where `T` is declared with
`HALCYON_REFLECT`. Field order in the macro matches the `?` positions in the SQL:

```cpp
struct Event {
    std::int64_t ts_ms;
    std::string  kind;
    std::string  payload;
};
HALCYON_REFLECT(Event, ts_ms, kind, payload);

std::vector<Event> events = collect_events();
auto r = db.executeBatch(
    "INSERT INTO events(ts_ms, kind, payload) VALUES (?, ?, ?)",
    halcyon::batchOf(events));

if (!r.ok())
    std::cerr << "batch failed: " << r.error().message << "\n";
else
    std::cout << r.value() << " rows inserted\n";
```

`batchOf` also accepts an `std::initializer_list<T>` for small inline sets:

```cpp
db.executeBatch(
    "INSERT INTO config(key, value) VALUES (?, ?)",
    halcyon::batchOf<Config>({{.key="debug", .value="0"},
                              {.key="timeout", .value="30"}}));
```

## `batchOf` from explicit tuples

Use tuple rows when you do not have a reflected struct, or when you want to
reorder/skip fields without changing the struct:

```cpp
using Row = std::tuple<std::int64_t, std::string, double>;
auto r = db.executeBatch(
    "INSERT INTO readings(sensor_id, label, value) VALUES (?, ?, ?)",
    halcyon::batchOf<std::int64_t, std::string, double>(
        {
            {1, "temp", 22.5},
            {2, "humidity", 60.0},
            {3, "pressure", 1013.2},
        }));
```

## Building a `Batch` incrementally

Construct a `Batch` yourself when rows are not all known upfront:

```cpp
halcyon::Batch batch;
while (source.has_next()) {
    auto row = source.next();
    batch.rows.push_back({
        halcyon::detail::cli::Value{row.id},
        halcyon::detail::cli::Value{row.name},
    });
}
db.executeBatch("INSERT INTO items(id, name) VALUES (?, ?)", batch);
```

## Error handling and retry

`executeBatch` runs each row individually — not as a server-side array bind.
The total affected-row count is returned. On a mid-batch failure, the count
reflects rows inserted before the failure; rows after the failure are not attempted.

`executeBatch` has no auto-retry. If the connection dies mid-batch, the returned
`Result` carries `ErrorCode::Connection`. Retry the whole batch from your code —
not just the failed row — or use a transaction to make the whole batch atomic:

```cpp
auto r = db.transaction([&](halcyon::Transaction& tx) -> halcyon::Result<void> {
    for (const auto& e : events) {
        auto res = tx.execute(
            "INSERT INTO events(ts_ms, kind) VALUES (?, ?)", e.ts_ms, e.kind);
        if (!res.ok()) return res.error();
    }
    return {};
});
```

## Performance note

For very large imports (millions of rows), consider using Db2's native `LOAD`
utility or `IMPORT` command instead — they bypass logging and are orders of
magnitude faster than per-row `INSERT`. `executeBatch` is best for application-level
bulk writes of up to a few thousand rows where you need row-level error visibility
and transactional guarantees.
