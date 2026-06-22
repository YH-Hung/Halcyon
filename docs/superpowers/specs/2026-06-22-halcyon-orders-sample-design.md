# Halcyon Orders Sample — Design

**Date:** 2026-06-22
**Status:** Approved (brainstorming)
**Topic:** A comprehensive, self-contained sample project showing how to include
and apply Halcyon (query / insert / update) against a local Db2 Docker container.

## 1. Goal

Provide a standalone, copy-pasteable sample that a downstream user can follow to:

1. Stand up a local IBM Db2 (the existing `docker/docker-compose.yml`, `SAMPLE` db).
2. Create a simple-but-realistic schema and seed data via SQL scripts.
3. Consume Halcyon as an external dependency through `find_package(Halcyon)`.
4. Perform **query**, **insert**, and **update** — plus typed struct mapping,
   named parameters, a batch insert, exact `DECIMAL`/`DATE`/`TIMESTAMP` handling,
   and a transaction — in **two parallel styles** (object-oriented/throwing and
   functional/`Result<T>`) that implement identical logic.
5. Follow a documented, essential setup procedure end-to-end.

The sample is "simple but representative": a mini order system (customers and
orders, 1:N) that mirrors how real business apps use the library.

## 2. Non-goals

- Async (`queryAsync`/`executeAsync`) and connection-pool tuning — intentionally
  omitted to keep the sample focused (they have their own guide pages).
- Generated-key retrieval — avoided by using a natural primary key for orders, so
  inserts/updates stay simple and rely only on the public surface API.
- A deliberate-failure/error-injection path — error handling is shown idiomatically
  (one `try/catch` in the OO file; `.ok()`/`.error()` checks in the functional
  file) without contriving a failure.
- Wiring the sample into the Halcyon build (`HALCYON_BUILD_EXAMPLES`). It is a
  *standalone consumer project* with its own `CMakeLists.txt`, demonstrating real
  downstream usage — not an in-tree example that builds with the library.

## 3. Placement & layout

A new top-level `samples/` directory keeps the standalone consumer clearly
separate from the in-tree `examples/` (which build *with* the library):

```
samples/orders/
  README.md                 # essential setup procedure + walkthrough explanation
  CMakeLists.txt            # standalone; find_package(Halcyon REQUIRED); two targets
  load_sql.sh               # convenience: copies + runs schema.sql, seed.sql in the container
  sql/
    schema.sql              # DDL: drop + create CUSTOMERS, ORDERS
    seed.sql                # INSERT seed customers + orders
  src/
    orders_model.hpp        # SHARED: Customer/Order structs + HALCYON_REFLECT + SQL string constants
    orders_oo.cpp           # OO / throwing-style walkthrough
    orders_functional.cpp   # functional / Result-style walkthrough
```

## 4. Schema (mini order system, 1:N)

Table names are unqualified, so they land in the connecting user's default schema
(`DB2INST1`) inside the existing `SAMPLE` database. `schema.sql` is idempotent at
the script level: it drops the tables first (ignoring "does not exist" on a fresh
db) and recreates them, so re-running the setup is safe.

```sql
CREATE TABLE customers (
    customer_id INTEGER       NOT NULL PRIMARY KEY,   -- app-assigned in seed
    name        VARCHAR(100)  NOT NULL,
    email       VARCHAR(255),                          -- nullable -> std::optional<std::string>
    created_at  DATE          NOT NULL                 -- -> halcyon::Date
);

CREATE TABLE orders (
    order_no     VARCHAR(20)   NOT NULL PRIMARY KEY,   -- natural key, e.g. 'ORD-1001'
    customer_id  INTEGER       NOT NULL REFERENCES customers(customer_id),
    status       VARCHAR(20)   NOT NULL,               -- NEW | PAID | SHIPPED | CANCELLED
    total_amount DECIMAL(11,2) NOT NULL,               -- exact money -> halcyon::Decimal
    order_ts     TIMESTAMP     NOT NULL                -- -> halcyon::Timestamp
);
```

Coverage: `INTEGER`, `VARCHAR`, a nullable column, `DATE`, `TIMESTAMP`,
`DECIMAL`, and a foreign key.

`seed.sql` inserts ~3 customers and ~4 orders, e.g.:

- Customers `1` Ada (ada@example.com), `2` Linus (NULL email), `3` Grace
  (grace@example.com).
- Orders `ORD-1001`/`ORD-1002` for customer 1, `ORD-1003` for customer 2,
  `ORD-1004` for customer 3, with a mix of statuses and `DECIMAL` totals.

## 5. Shared model (`src/orders_model.hpp`)

To keep the two style variants in lock-step (no drift) and side-by-side
diffable, the reflected structs and every SQL string live exactly once here.

```cpp
#pragma once
#include <optional>
#include <string>
#include <halcyon/halcyon.hpp>

struct Customer {
    int                        customer_id;
    std::string                name;
    std::optional<std::string> email;       // nullable column
    halcyon::Date              created_at;
};
HALCYON_REFLECT(Customer, customer_id, name, email, created_at);

struct Order {
    std::string       order_no;
    int               customer_id;
    std::string       status;
    halcyon::Decimal  total_amount;
    halcyon::Timestamp order_ts;
};
HALCYON_REFLECT(Order, order_no, customer_id, status, total_amount, order_ts);

namespace sql {
// SELECT column order MUST match struct field declaration order for queryAs<T>.
inline constexpr const char* kSelectCustomers =
    "SELECT customer_id, name, email, created_at FROM customers ORDER BY customer_id";
inline constexpr const char* kSelectOrdersForCustomer =
    "SELECT order_no, customer_id, status, total_amount, order_ts "
    "FROM orders WHERE customer_id = :cid ORDER BY order_no";   // named param
inline constexpr const char* kInsertOrder =
    "INSERT INTO orders(order_no, customer_id, status, total_amount, order_ts) "
    "VALUES (?, ?, ?, ?, ?)";                                    // positional
inline constexpr const char* kUpdateOrderStatus =
    "UPDATE orders SET status = ? WHERE order_no = ?";
inline constexpr const char* kSelectOrderByNo =
    "SELECT order_no, customer_id, status, total_amount, order_ts "
    "FROM orders WHERE order_no = ?";
}  // namespace sql
```

DSN resolution (shared helper): read `HALCYON_TEST_DSN`; if unset, fall back to
the documented default
`DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;`.

## 6. Walkthrough logic (identical in both files)

Seven narrative steps, each a clearly commented section that prints output:

1. **Open** the database from the resolved DSN.
2. **Query (struct mapping):** select all customers into `std::vector<Customer>`
   via `queryAs<Customer>` and print them (shows `std::optional` email rendering).
3. **Query (named param + filter):** select orders for one customer using
   `params{{":cid", id}}`, mapped to `Order`; print them.
4. **Insert:** insert one new order via positional `?` binding, passing a
   `halcyon::Decimal` total and a `halcyon::Timestamp`; print affected rows.
5. **Batch insert:** insert several more orders via `executeBatch` + `batchOf({...})`
   (tuple rows); print the count.
6. **Update in a transaction:** atomically update an order's `status` to
   `'SHIPPED'` (and a second related status update) inside a single transaction,
   then commit. A short comment explains how returning/propagating an error would
   instead roll back.
7. **Re-query** the affected orders to display the post-insert/update state
   (read-after-write), proving the changes landed.

To allow safe re-runs, the inserts use fixed new order numbers (e.g. `ORD-2001`,
`ORD-2002`, ...) and the program first deletes any rows it is about to insert (a
brief "reset demo rows" step), so the sample is rerunnable without re-seeding.

### 6.1 OO / throwing style (`orders_oo.cpp`)

Uses the object-oriented throwing API inside one `try/catch (const halcyon::Exception&)`:

- `halcyon::Database::openOrThrow(dsn)`
- `db.queryAsOrThrow<Customer>(sql::kSelectCustomers)`
- `db.queryAsOrThrow<Order>(sql::kSelectOrdersForCustomer, halcyon::params{{":cid", id}})`
- `db.executeOrThrow(sql::kInsertOrder, order_no, customer_id, status, total, ts)`
- `db.executeBatch(sql::kInsertOrder, halcyon::batchOf({...}))` (returns `Result`;
  `.value()` unwraps)
- `db.transaction([&](halcyon::Transaction& tx) -> halcyon::Result<...> { ... })`
  for step 6, checked via the returned `Result`.

### 6.2 Functional / `Result<T>` style (`orders_functional.cpp`)

Uses the free-function API with explicit `.ok()`/`.error()` checks, no exceptions:

- `auto dbr = halcyon::Database::open(dsn); if (!dbr.ok()) { ...; return 1; }`
- `halcyon::query_as<Customer>(db, sql::kSelectCustomers)`
- `halcyon::query_as<Order>(db, sql::kSelectOrdersForCustomer, halcyon::params{{":cid", id}})`
- `halcyon::execute(db, sql::kInsertOrder, ...)`
- `db.executeBatch(sql::kInsertOrder, halcyon::batchOf({...}))`
- `halcyon::transaction(db, [&](halcyon::Transaction& tx) -> halcyon::Result<...> { ... })`

Both files print the same labelled output so they can be diffed and produce
identical results.

## 7. Build (`samples/orders/CMakeLists.txt`)

```cmake
cmake_minimum_required(VERSION 3.20)
project(halcyon_orders_sample CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Halcyon REQUIRED)

add_executable(orders_oo         src/orders_oo.cpp)
add_executable(orders_functional src/orders_functional.cpp)
target_link_libraries(orders_oo         PRIVATE halcyon::halcyon)
target_link_libraries(orders_functional PRIVATE halcyon::halcyon)
```

Consumer notes (mirroring `tests/smoke/` and `HalcyonConfig.cmake.in`):

- Configure with `-DCMAKE_PREFIX_PATH=<halcyon-install-or-build-tree>` so
  `find_package(Halcyon)` resolves.
- Because the vendored-driver default root is relative to the *consumer's* source
  tree, the consumer must point at Halcyon's driver:
  `-DDB2_CLIDRIVER_ROOT=<halcyon-repo>/third_party/clidriver` (or set
  `DB2CLI_INCLUDE_DIR`/`DB2CLI_LIBRARY`). Halcyon adds the driver lib dir to the
  RPATH, so the sample runs without `DYLD_LIBRARY_PATH`/`LD_LIBRARY_PATH`.

## 8. Setup procedure (`README.md`)

The README documents the complete essential procedure:

1. **Prerequisites:** Docker (or Apple `container`), a built/installed Halcyon,
   and the vendored CLI driver under `third_party/clidriver` (incl. the macOS
   one-time `xattr` quarantine clear from `AGENTS.md`/`docker/README.md`).
2. **Start Db2:** `docker compose -f docker/docker-compose.yml up -d`; wait until
   `... ps` shows `healthy` (first boot creates `SAMPLE`).
3. **Load schema + seed:** run `samples/orders/load_sql.sh`, which
   `docker compose cp`s `sql/schema.sql` and `sql/seed.sql` into the container and
   runs them with `db2 -tvf`. The raw equivalent commands are shown too, e.g.:
   ```bash
   docker compose -f docker/docker-compose.yml cp samples/orders/sql/schema.sql db2:/tmp/schema.sql
   docker compose -f docker/docker-compose.yml exec db2 \
     su - db2inst1 -c "db2 connect to SAMPLE && db2 -tvf /tmp/schema.sql"
   ```
   An Apple `container` variant note is included (no Compose; `container cp` +
   `container exec ... su - db2inst1 -c '. ~/sqllib/db2profile; db2 ...'`).
4. **Build the sample:**
   ```bash
   cmake -S samples/orders -B samples/orders/build \
     -DCMAKE_PREFIX_PATH=<halcyon-install-or-build-tree> \
     -DDB2_CLIDRIVER_ROOT=<halcyon-repo>/third_party/clidriver
   cmake --build samples/orders/build -j
   ```
5. **Run:**
   ```bash
   export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
   ./samples/orders/build/orders_oo
   ./samples/orders/build/orders_functional
   ```
6. **Expected output:** an annotated transcript (customers, orders, insert/batch
   counts, post-update state) — identical between the two binaries.
7. **Teardown:** `docker compose -f docker/docker-compose.yml down`.

## 9. Verification

Per `AGENTS.md`, verification is end-to-end against live Db2 (not skipped):

1. Build Halcyon and either install it to a prefix or use the build tree.
2. `docker compose up -d`; wait for `healthy`.
3. Run `load_sql.sh` (schema + seed apply cleanly).
4. Configure + build `samples/orders` against the Halcyon tree with
   `DB2_CLIDRIVER_ROOT` set.
5. Run both `orders_oo` and `orders_functional`; confirm they complete with exit
   code 0 and print the expected, identical output.
6. Re-run both binaries to confirm rerunnability (the demo-row reset works).
7. `docker compose down`.

## 10. Risks / open considerations

- **`queryAs<T>` column/field order:** mapping is positional, so each `SELECT`
  must list columns in struct field order. Centralizing SQL in `orders_model.hpp`
  next to the structs mitigates drift.
- **`DECIMAL`/date text form:** `halcyon::Decimal`/`Date`/`Timestamp` carry the
  driver's text verbatim; printed values reflect Db2's formatting (e.g.
  `1234.50`, `2026-06-22-...`). The README notes this is expected.
- **Driver root for consumers:** the most common setup mistake is omitting
  `DB2_CLIDRIVER_ROOT`; the README calls it out explicitly.
