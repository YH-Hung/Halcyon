# Batch Operations

`Database::executeBatch` inserts multiple rows with a single prepared statement
using Db2 CLI **array binding**: rows are bound column-wise and sent to the
server in one `SQLExecute` per chunk, returning the total affected-row count.

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

`executeBatch` binds the rows column-wise and executes them with Db2 CLI array
binding (one `SQLExecute` per chunk). On a row failure the call returns a single
classified `Error` (e.g. a unique-constraint violation surfaces as
`ErrorCode::Constraint`, SQLSTATE 23505). Each chunk is atomic — a failed chunk
commits nothing — so under autocommit the returned count reflects whole chunks
that completed before the failure. Db2 does not report which row in the chunk
failed, so wrap the call in a transaction for all-or-nothing and re-drive the
whole batch on error.

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

## Performance & large batches

Array binding collapses N per-row round-trips into one execute per ~16 MiB
chunk. For throughput, wrap the batch in a transaction (`db.begin()` →
`executeBatch` → `commit`) so the whole load commits once instead of once per
chunk.

For very large all-or-nothing loads, a single transaction holds row locks and
transaction-log space until commit, which can trigger lock escalation or a full
transaction log. Chunking bounds client memory, not server log/lock pressure —
so for multi-million-row loads, split into several independently-committed
`executeBatch` calls, trading strict whole-load atomicity for bounded log use.
For the very largest imports, Db2's native `LOAD` utility or `IMPORT` command
bypasses logging entirely and is faster still.
