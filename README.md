# Halcyon

A modern **C++17** client library for **IBM Db2**, built on the IBM Db2 CLI (`sqlcli1.h`). Halcyon provides a high-level, ergonomic, type-safe API in both object-oriented/fluent and functional styles — connection pooling, transparent reconnect, safe auto-retry on transient failures, `std::future` async, and optional observability via Prometheus and OpenTelemetry.

## Features

- **High-level API** — one connection string and you're ready to query
- **Dual error model** — `Result<T>` (functional) and throwing overloads; your choice per call site
- **Named & anonymous parameters** — `?` positional or `:name` named params
- **Type-safe row mapping** — tuple unpacking, opt-in struct reflection via `HALCYON_REFLECT`
- **Connection pooling** — min/max bounds, acquire timeout, validation, idle/lifetime reaping
- **Transparent reconnect & safe auto-retry** — recovers from transient failures without caller involvement
- **Async** — `std::future`-based `queryAsync`/`executeAsync`; coroutine-ready for a future C++20 layer
- **RAII transactions** — guard and functional `transaction(...)` helper
- **Bulk/batch insert** — `executeBatch` with array parameters (v1 in-scope)
- **Optional observability** — Prometheus metrics and OpenTelemetry tracing, zero overhead when disabled
- **Mockable seam** — `ICliDriver` interface keeps all IBM CLI details behind a single boundary; unit tests need no live database

## Requirements

| Requirement | Version |
|---|---|
| C++ standard | C++17 |
| CMake | ≥ 3.20 |
| IBM Db2 CLI driver | vendored at `third_party/clidriver` (or override via `DB2_CLIDRIVER_ROOT`) |
| Platforms | Linux x86_64, macOS |
| Test framework | GoogleTest (fetched by CMake) |
| Metrics (optional) | `prometheus-cpp` |
| Tracing (optional) | `opentelemetry-cpp` |

## Quick start

```cpp
#include <halcyon/halcyon.hpp>
using namespace halcyon;

// Open a pooled database connection
auto db = Database::openOrThrow(
    "DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=secret;",
    PoolConfig{.min = 2, .max = 16, .acquireTimeout = std::chrono::seconds(5)});

// Query with anonymous parameters
auto rows = db.query("SELECT id, name FROM users WHERE age > ? AND city = ?", 21, "NYC");
for (auto& row : rows.value()) {
    auto [id, name] = row.as<int, std::string>();
}

// Query with named parameters
auto rows2 = db.query("SELECT id, name FROM users WHERE age > :age",
                      params{{"age", 21}});

// Struct mapping
struct User { int id; std::string name; };
HALCYON_REFLECT(User, id, name);
auto users = db.queryAs<User>("SELECT id, name FROM users"); // Result<std::vector<User>>

// Functional style
auto users2 = query_as<User>(db, "SELECT id, name FROM users WHERE id = :id",
                             params{{"id", 7}});

// Async
std::future<Result<std::vector<User>>> f = db.queryAsync<User>("SELECT id, name FROM users");

// Transaction — functional (commits on success, rolls back on error)
db.transaction([&](Transaction& tx) {
    tx.execute("INSERT INTO orders(product_id, qty) VALUES (?, ?)", 42, 3);
    tx.execute("UPDATE stock SET qty = qty - ? WHERE product_id = ?", 3, 42);
});

// Transaction — RAII (auto-rollback unless committed)
auto tx = db.begin();
tx.execute("UPDATE account SET balance = balance - ? WHERE id = ?", 100, 1);
tx.execute("UPDATE account SET balance = balance + ? WHERE id = ?", 100, 2);
tx.commit();

// Bulk insert
db.executeBatch("INSERT INTO events(ts, kind) VALUES (?,?)", batchOf(eventRows));
```

## Build

```bash
# Configure (tests enabled by default)
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON

# Build
cmake --build build -j

# Run unit tests (no live database needed)
ctest --test-dir build --output-on-failure

# Run integration tests (requires a live Db2 — see below)
ctest --test-dir build -L integration --output-on-failure
```

### Integration tests against live Db2

The integration suite (`tests/integration/`, CTest label `integration`) only
*runs* when `HALCYON_TEST_DSN` is set; otherwise the tests report as skipped.
Configure with `-DHALCYON_BUILD_INTEGRATION_TESTS=ON` to build them.

**Docker (default):**

```bash
docker compose -f docker/docker-compose.yml up -d
docker compose -f docker/docker-compose.yml ps   # wait for STATUS = healthy (~2 min)
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
ctest --test-dir build -L integration --output-on-failure
docker compose -f docker/docker-compose.yml down
```

**Apple `container` (Docker alternative on macOS):** see
[`docker/README.md`](docker/README.md#apple-container-docker-alternative-on-macos).

### CMake options

| Option | Default | Description |
|---|---|---|
| `HALCYON_BUILD_TESTS` | `ON` | Build unit and integration tests |
| `HALCYON_BUILD_EXAMPLES` | `OFF` | Build example programs |
| `HALCYON_WITH_PROMETHEUS` | `OFF` | Compile Prometheus metrics adapter |
| `HALCYON_WITH_OTEL` | `OFF` | Compile OpenTelemetry tracing adapter |
| `HALCYON_WARNINGS_AS_ERRORS` | `OFF` | Treat compiler warnings as errors |
| `DB2_CLIDRIVER_ROOT` | `third_party/clidriver` | Path to IBM Db2 CLI driver |

The CLI driver library directory is added to the RPATH automatically, so tests and examples run without setting `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH`.

### Using Halcyon in your CMake project

```cmake
find_package(Halcyon REQUIRED)
target_link_libraries(my_app PRIVATE halcyon::halcyon)
```

## Architecture

Halcyon uses a strict layered architecture with a **thin CLI seam** — only code under `src/detail/cli/` ever includes `sqlcli1.h`. Everything above the seam is portable, mockable C++17.

```
facade ─► pool ─► core ─► detail::cli ─► sqlcli1.h
   │                          ▲
   └──► observability ────────┘   (interfaces only; adapters optional)
```

| Layer | Responsibility |
|---|---|
| `detail::cli` | RAII wrappers over CLI handles; translates `SQLRETURN`/SQLSTATE → `halcyon::Error` |
| `core` | `Connection`, `Statement`, `ResultSet`, parameter binding, type mapping, transactions |
| `pool` | `ConnectionPool` — thread-safe acquire/release, validation, reconnect, retry, async executor |
| `facade` | `Database` high-level entry point, free functions, observability hook injection |

## Repository layout

```
include/halcyon/          Public headers (umbrella: halcyon.hpp)
  detail/cli/             ICliDriver seam (mockable; no sqlcli1.h in public headers)
src/
  detail/cli/             Db2CliDriver — only code that includes sqlcli1.h
  core/                   Connection / Statement / ResultSet / binding / types
  pool/                   ConnectionPool, reconnect, retry, async executor
  facade/                 Database impl, free functions
  observability/          prometheus_adapter.cpp, otel_adapter.cpp (CMake-gated)
tests/
  unit/                   GoogleTest; MockCliDriver — no live DB required
  integration/            Dockerized Db2 (CTest label "integration")
cmake/                    FindDB2CLI.cmake, package config
third_party/clidriver/    Vendored IBM Db2 CLI driver (gitignored)
examples/                 OO and functional usage samples
docker/                   Db2 Compose for integration tests
docs/superpowers/
  specs/                  Design specification
  plans/                  Implementation plans
```

## Error model

Every API call is available in two styles:

```cpp
// Result<T> style — explicit error handling, no exceptions
Result<std::vector<User>> r = db.queryAs<User>("SELECT ...");
if (!r) { std::cerr << r.error().message; return; }
process(r.value());

// Throwing style — throws halcyon::Exception subclass on failure
auto users = db.queryAsOrThrow<User>("SELECT ...");
```

`Error` carries:
- `ErrorCode` enum (`Connection`, `Timeout`, `Constraint`, `Syntax`, `Deadlock`, `Transient`, `Mapping`, `Pool`, `Unknown`)
- Raw 5-char SQLSTATE string
- Native Db2 SQLCODE
- Human-readable message from `SQLGetDiagRec`
- `retriable` flag (drives transparent auto-retry)

## Observability

When built with the optional adapters, Halcyon emits the following Prometheus metrics:

| Metric | Type | Description |
|---|---|---|
| `halcyon_queries_total{op,status}` | Counter | Total queries by operation and outcome |
| `halcyon_query_duration_seconds{op}` | Histogram | Query latency |
| `halcyon_pool_connections{state}` | Gauge | Idle / active connection counts |
| `halcyon_pool_acquire_wait_seconds` | Histogram | Time waiting for a pooled connection |
| `halcyon_reconnects_total` | Counter | Transparent reconnect events |
| `halcyon_retries_total{outcome}` | Counter | Auto-retry events |
| `halcyon_errors_total{code}` | Counter | Errors by `ErrorCode` |

OpenTelemetry spans: `halcyon.query`, `halcyon.execute`, `halcyon.transaction`, `halcyon.acquire`, `halcyon.reconnect` — with `db.system=db2`, `db.statement`, `db.rows_affected`, `error.sqlstate` attributes.

## Contributing

- Follow TDD: write a failing test first, implement the minimum to pass, then commit.
- Conventional commits: `feat:`, `fix:`, `build:`, `docs:`, `chore:`, `test:`.
- Code must be clean under `-Wall -Wextra -Wpedantic`; run `clang-format` and `clang-tidy` before committing.
- Do not leak `sqlcli1.h` types above the `detail::cli` seam.
- Do not add hard dependencies on `prometheus-cpp` or `opentelemetry-cpp` in core code.
- See `AGENTS.md` for full conventions and `docs/superpowers/specs/` for the design spec.

## License

*(License to be determined.)*
