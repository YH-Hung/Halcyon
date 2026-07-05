# Getting Started

## Requirements

| Requirement | Version |
|---|---|
| C++ standard | C++17 |
| CMake | ≥ 3.20 |
| IBM Db2 CLI driver | user-supplied at `third_party/clidriver`, or set `DB2_CLIDRIVER_ROOT` |
| GoogleTest | fetched automatically by CMake |

## Add Halcyon to your project

After installing Halcyon (or pointing `CMAKE_PREFIX_PATH` at the build tree):

```cmake
find_package(Halcyon REQUIRED)
target_link_libraries(my_app PRIVATE halcyon::halcyon)
```

Include the single umbrella header in your source:

```cpp
#include <halcyon/halcyon.hpp>
```

## Open a database

`Database::open` (returns `Result<Database>`) and `Database::openOrThrow` (throws on failure) both accept a Db2 connection string and an optional `PoolConfig`:

```cpp
// Throwing — simplest path
auto db = halcyon::Database::openOrThrow(
    "DATABASE=MYDB;HOSTNAME=db.example.com;PORT=50000;UID=myuser;PWD=secret;");

// Result<T> — no exceptions
auto result = halcyon::Database::open(
    "DATABASE=MYDB;HOSTNAME=db.example.com;PORT=50000;UID=myuser;PWD=secret;");
if (!result.ok()) {
    std::cerr << result.error().message << "\n";
    return 1;
}
halcyon::Database db = std::move(result.value());
```

`Database` is a copyable handle: copies share one underlying pool (via `shared_ptr`). Pass it by value across threads freely.

## First query

```cpp
#include <halcyon/halcyon.hpp>
#include <iostream>

int main() {
    auto db = halcyon::Database::openOrThrow(
        "DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=secret;");

    // query() returns Result<QueryResult>; iterate with a range-for.
    // Each row is read lazily from the cursor.
    auto res = db.queryOrThrow("SELECT tabname, card FROM syscat.tables WHERE tabschema = ?",
                               std::string("DB2INST1"));
    for (auto& row : res) {
        auto [name, card] = row.as<std::string, std::int64_t>();
        std::cout << name << " has " << card << " rows\n";
    }
}
```

Key points:
- `queryOrThrow` returns a `QueryResult`; it owns the live cursor and the pooled connection.
- Destroy or let `QueryResult` go out of scope to release the connection back to the pool.
- The connection is returned automatically — no explicit close.

## Execute a DML statement

```cpp
std::int64_t affected = db.executeOrThrow(
    "UPDATE inventory SET qty = qty - ? WHERE product_id = ?", 3, 42);
std::cout << affected << " row(s) updated\n";
```

`execute` returns the number of affected rows; `executeOrThrow` unwraps or throws.

## Build and run the examples

```bash
cmake -S . -B build -DHALCYON_BUILD_EXAMPLES=ON
cmake --build build --target oo_usage functional_usage -j

export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=secret;"
./build/examples/oo_usage
./build/examples/functional_usage
```

## Next steps

- [Error Handling](error-handling.md) — choosing between `Result<T>` and exceptions
- [Parameters & Types](parameters-and-types.md) — binding values and mapping rows to structs
- [Connection Pool](connection-pool.md) — tuning pool size, timeouts, and reconnect behaviour
