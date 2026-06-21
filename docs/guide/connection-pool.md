# Connection Pool

`Database::open` creates and manages a `ConnectionPool` internally. You configure
it via `PoolConfig`.

## `PoolConfig` reference

```cpp
halcyon::PoolConfig config{
    .min                    = 2,                           // warm connections to maintain
    .max                    = 16,                          // hard ceiling
    .acquireTimeout         = std::chrono::seconds(5),     // how long acquire() waits
    .idleTimeout            = std::chrono::minutes(10),    // evict idle connections after
    .maxLifetime            = std::chrono::minutes(30),    // evict connections regardless of use
    .validateOnAcquire      = false,                       // ping before handing out (see below)
    .backoff                = {                            // reconnect backoff
        .baseDelay  = std::chrono::milliseconds(50),
        .maxDelay   = std::chrono::milliseconds(2000),
        .maxAttempts = 3,
    },
    .maintenanceInterval    = std::chrono::milliseconds(1000), // reaper tick period
    .startMaintenanceThread = true,                        // false = no background thread
    .statementCacheSize     = 64,                          // LRU prepared-statement cache; 0 = off
    .observability          = {},                          // Prometheus / OTel wiring (see Observability)
};

auto db = halcyon::Database::openOrThrow(dsn, config);
```

| Field | Default | Description |
|---|---|---|
| `min` | `1` | Minimum connections. Opened eagerly at startup. |
| `max` | `8` | Maximum connections. Acquired lazily up to this count. |
| `acquireTimeout` | `5 000 ms` | `Pool` error if no connection becomes free in time. |
| `idleTimeout` | `60 000 ms` | Connections idle longer than this are closed and removed. |
| `maxLifetime` | `1 800 000 ms` | Maximum total connection age, regardless of use. Prevents stale server-side state. |
| `validateOnAcquire` | `false` | Send a cheap `SELECT 1` before handing out each connection. Adds latency but catches dead connections immediately. |
| `backoff.baseDelay` | `50 ms` | Initial reconnect sleep. Doubles per attempt. |
| `backoff.maxDelay` | `2 000 ms` | Cap on reconnect sleep. |
| `backoff.maxAttempts` | `3` | Maximum reconnect/retry attempts per operation. |
| `maintenanceInterval` | `1 000 ms` | How often the background reaper checks idle/lifetime limits. |
| `startMaintenanceThread` | `true` | Set to `false` in tests to avoid background threads. |
| `statementCacheSize` | `64` | Per-connection LRU prepared-statement slots. See [Advanced](advanced.md#statement-cache). |
| `observability` | (null) | Wire Prometheus / OTel here. See [Observability](observability.md). |

## Sizing guidelines

| Scenario | Recommendation |
|---|---|
| Low-traffic service (< 50 req/s) | `min=1, max=4` |
| Web server with burst traffic | `min=4, max=max_threads` — never more than thread count |
| Long-running batch job | `min=1, max=1` — serialised is fine; no contention |
| High-concurrency microservice | `min=½·max_threads, max=max_threads` |

A pool larger than your thread count wastes Db2 server resources. Db2 assigns
agent threads to connections; too many open connections hurts the server as well
as your process.

## Auto-retry

Halcyon classifies every error as retriable or non-retriable based on SQLSTATE:

- **Connection errors** (`ErrorCode::Connection`) and **transient errors**
  (`ErrorCode::Transient`) are retriable.
- `Constraint`, `Syntax`, `Mapping`, and `Pool` errors are **never retried**.

Auto-retry behaviour by call type:

| Call | Default policy |
|---|---|
| `query` / `queryAs` (SELECT, VALUES, WITH…SELECT) | Up to `backoff.maxAttempts` replays |
| `execute` with DML (INSERT / UPDATE / DELETE / MERGE) | **One attempt only** — not safe to replay automatically |
| `execute` with DDL or other non-DML | One attempt |
| Any call with an explicit `ExecPolicy` | As configured |

To opt a write into auto-retry (safe when it is truly idempotent):

```cpp
halcyon::ExecPolicy policy = halcyon::ExecPolicy::idempotent(5);
policy.backoff = halcyon::BackoffPolicy{
    .baseDelay   = std::chrono::milliseconds(100),
    .maxDelay    = std::chrono::milliseconds(5000),
    .maxAttempts = 5,
};
db.execute("INSERT OR REPLACE INTO kv(key, value) VALUES (?, ?)",
           policy,
           std::string("greeting"), std::string("hello"));
```

## Validation on acquire

`validateOnAcquire = true` sends `SELECT 1 FROM SYSIBM.SYSDUMMY1` to the
connection before handing it to the caller. The extra round-trip (~0.5 ms over
LAN) eliminates stale connections being handed to callers. Consider it when:

- You expect long idle periods between calls.
- A network device silently kills idle TCP connections.
- You cannot tolerate even the first failure from a dead connection.

Leave it `false` (the default) for latency-sensitive paths — transparent reconnect
handles the stale-connection case automatically.

## Direct pool access

The raw pool is accessible if you need metrics or direct control:

```cpp
halcyon::ConnectionPool& pool = db.pool();
```

`ConnectionPool` is non-copyable and non-movable. It is kept alive by the
`Database`'s internal `shared_ptr`; do not store raw references beyond the
`Database`'s lifetime.
