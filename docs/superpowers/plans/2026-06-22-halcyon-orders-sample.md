# Halcyon Orders Sample Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone consumer sample at `samples/orders/` that demonstrates Halcyon query/insert/update against a local Db2 container, in parallel OO (throwing) and functional (`Result<T>`) styles sharing one model + SQL schema/seed.

**Architecture:** A self-contained CMake project that consumes Halcyon via `find_package(Halcyon)` (not wired into the Halcyon build). Two executables (`orders_oo`, `orders_functional`) run identical seven-step logic over a shared `orders_model.hpp` (reflected structs + SQL strings). Schema and seed live in `.sql` scripts loaded into the existing `docker/docker-compose.yml` Db2 via a `load_sql.sh` helper. Verification is end-to-end against live Db2, per `AGENTS.md`.

**Tech Stack:** C++17, Halcyon, CMake ≥ 3.20, IBM Db2 CLI (vendored `third_party/clidriver`), Docker Db2 (`icr.io/db2_community/db2`).

**Spec:** `docs/superpowers/specs/2026-06-22-halcyon-orders-sample-design.md`

---

## Conventions for this plan

This is a runnable sample, not a unit-tested library component, so the
"test" in each TDD cycle is a **concrete build/run with expected output**.
Treat a failing build or a non-zero exit / wrong output as a red test.

Prerequisite for any task that builds or runs (Tasks 3, 7+): a built Halcyon
tree (or install prefix) and the vendored driver. Define these shell variables
once per session and reuse them:

```bash
# From the repo root. Adjust HALCYON_BUILD if you built elsewhere.
export REPO_ROOT="$(pwd)"
export HALCYON_BUILD="${REPO_ROOT}/build"          # an existing Halcyon build tree
export DRIVER_ROOT="${REPO_ROOT}/third_party/clidriver"
export SAMPLE_DIR="${REPO_ROOT}/samples/orders"
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
```

If `${HALCYON_BUILD}` does not exist yet, create it first:

```bash
cmake -S "${REPO_ROOT}" -B "${HALCYON_BUILD}"
cmake --build "${HALCYON_BUILD}" -j
```

---

## File structure

- Create: `samples/orders/CMakeLists.txt` — standalone project; two targets.
- Create: `samples/orders/src/orders_model.hpp` — shared structs, SQL, helpers.
- Create: `samples/orders/src/orders_oo.cpp` — OO/throwing walkthrough.
- Create: `samples/orders/src/orders_functional.cpp` — functional walkthrough.
- Create: `samples/orders/sql/schema.sql` — DDL.
- Create: `samples/orders/sql/seed.sql` — seed data.
- Create: `samples/orders/load_sql.sh` — loads schema+seed into the container.
- Create: `samples/orders/README.md` — full setup procedure + expected output.
- Modify: `README.md` — add `samples/` to the repo layout block.

---

## Task 1: SQL schema and seed scripts

**Files:**
- Create: `samples/orders/sql/schema.sql`
- Create: `samples/orders/sql/seed.sql`

- [x] **Step 1: Write `samples/orders/sql/schema.sql`**

```sql
-- Halcyon orders sample — schema.
-- Apply with: db2 connect to SAMPLE && db2 -tvf schema.sql
-- On a fresh database the two DROP statements report SQL0204N
-- ("name is an undefined name"); that is harmless — db2 -tvf continues.

DROP TABLE orders;
DROP TABLE customers;

CREATE TABLE customers (
    customer_id INTEGER       NOT NULL PRIMARY KEY,
    name        VARCHAR(100)  NOT NULL,
    email       VARCHAR(255),
    created_at  DATE          NOT NULL
);

CREATE TABLE orders (
    order_no     VARCHAR(20)   NOT NULL PRIMARY KEY,
    customer_id  INTEGER       NOT NULL REFERENCES customers(customer_id),
    status       VARCHAR(20)   NOT NULL,
    total_amount DECIMAL(11,2) NOT NULL,
    order_ts     TIMESTAMP     NOT NULL
);
```

- [x] **Step 2: Write `samples/orders/sql/seed.sql`**

```sql
-- Halcyon orders sample — seed data. Apply after schema.sql:
-- db2 connect to SAMPLE && db2 -tvf seed.sql

INSERT INTO customers(customer_id, name, email, created_at) VALUES
    (1, 'Ada Lovelace',   'ada@example.com',   '2026-01-15'),
    (2, 'Linus Torvalds',  NULL,               '2026-02-03'),
    (3, 'Grace Hopper',   'grace@example.com', '2026-03-21');

INSERT INTO orders(order_no, customer_id, status, total_amount, order_ts) VALUES
    ('ORD-1001', 1, 'PAID',     '199.99', '2026-04-01 10:00:00'),
    ('ORD-1002', 1, 'NEW',       '49.50', '2026-04-02 11:30:00'),
    ('ORD-1003', 2, 'SHIPPED',  '320.00', '2026-04-03 09:15:00'),
    ('ORD-1004', 3, 'CANCELLED', '15.00', '2026-04-04 14:45:00');
```

- [x] **Step 3: Sanity-check the SQL files exist and parse visually**

These are applied against the live container in Task 2; here just confirm both
files were written and the column lists match the spec schema. No command.

- [x] **Step 4: Commit**

```bash
git add samples/orders/sql/schema.sql samples/orders/sql/seed.sql
git commit -m "feat: add orders sample schema and seed SQL"
```

---

## Task 2: SQL loader script and live load

**Files:**
- Create: `samples/orders/load_sql.sh`

- [x] **Step 1: Write `samples/orders/load_sql.sh`**

```bash
#!/usr/bin/env bash
# Loads the orders sample schema + seed into the Db2 SAMPLE database running in
# the docker-compose container (docker/docker-compose.yml). Run from anywhere;
# paths resolve relative to this script.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
COMPOSE=(docker compose -f "${REPO_ROOT}/docker/docker-compose.yml")
SERVICE="db2"

for f in schema.sql seed.sql; do
    echo "==> loading ${f}"
    "${COMPOSE[@]}" cp "${SCRIPT_DIR}/sql/${f}" "${SERVICE}:/tmp/${f}"
    "${COMPOSE[@]}" exec -T "${SERVICE}" \
        su - db2inst1 -c "db2 connect to SAMPLE && db2 -tvf /tmp/${f}"
done

echo "==> done"
```

- [x] **Step 2: Make it executable**

Run: `chmod +x samples/orders/load_sql.sh`
Expected: no output, exit 0.

- [x] **Step 3: Bring up Db2 and load (red→green: tables must end up populated)**

Run:
```bash
docker compose -f docker/docker-compose.yml up -d
# wait until STATUS shows healthy (first boot creates SAMPLE):
docker compose -f docker/docker-compose.yml ps
./samples/orders/load_sql.sh
```
Expected: `load_sql.sh` prints `loading schema.sql`, `loading seed.sql`, `done`;
the `DROP` lines may print `SQL0204N` on first run (harmless); the `INSERT`s
report `DB20000I` / number of rows.

- [x] **Step 4: Verify the data landed**

Run:
```bash
docker compose -f docker/docker-compose.yml exec -T db2 \
  su - db2inst1 -c "db2 connect to SAMPLE && db2 -x 'SELECT COUNT(*) FROM customers' && db2 -x 'SELECT COUNT(*) FROM orders'"
```
Expected: `3` then `4`.

- [x] **Step 5: Commit**

```bash
git add samples/orders/load_sql.sh
git commit -m "feat: add orders sample SQL loader script"
```

---

## Task 3: Standalone CMake project + connectivity smoke

This task scaffolds the build with a minimal `orders_oo.cpp` that only opens the
database, proving `find_package(Halcyon)` + driver wiring + live connect work
before the full walkthrough is written. The minimal file is replaced in Task 5.

**Files:**
- Create: `samples/orders/CMakeLists.txt`
- Create: `samples/orders/src/orders_oo.cpp` (temporary minimal version)

- [x] **Step 1: Write `samples/orders/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)
project(halcyon_orders_sample CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Consumes Halcyon as an installed/exported package. Configure with:
#   -DCMAKE_PREFIX_PATH=<halcyon build-or-install tree>
#   -DDB2_CLIDRIVER_ROOT=<halcyon repo>/third_party/clidriver
# (the vendored-driver default root is relative to THIS project, so point it at
#  Halcyon's driver explicitly.)
find_package(Halcyon REQUIRED)

add_executable(orders_oo         src/orders_oo.cpp)
add_executable(orders_functional src/orders_functional.cpp)
target_link_libraries(orders_oo         PRIVATE halcyon::halcyon)
target_link_libraries(orders_functional PRIVATE halcyon::halcyon)
```

- [x] **Step 2: Write a temporary minimal `samples/orders/src/orders_oo.cpp`**

This uses `query` + `Row::as<>` (not `queryAs<T>`, which requires a
`HALCYON_REFLECT`'d struct — a bare scalar is not reflected). It mirrors the
existing `examples/oo_usage.cpp` connectivity check.

```cpp
// Temporary connectivity smoke; replaced by the full walkthrough in Task 5.
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include <halcyon/halcyon.hpp>

int main() {
    const char* env = std::getenv("HALCYON_TEST_DSN");
    const std::string dsn =
        env ? env
            : "DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;";
    try {
        auto db = halcyon::Database::openOrThrow(dsn);
        auto rows = db.queryOrThrow("SELECT 1 FROM SYSIBM.SYSDUMMY1");
        for (auto& row : rows) {
            std::cout << "connected, value="
                      << std::get<0>(row.as<std::int64_t>()) << "\n";
        }
    } catch (const halcyon::Exception& e) {
        std::cerr << "halcyon error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
```

- [x] **Step 3: Create the temporary functional stub so CMake configures**

The CMake lists two executables; create a matching minimal
`samples/orders/src/orders_functional.cpp` (replaced in Task 6):

```cpp
// Temporary stub; replaced by the full walkthrough in Task 6.
#include <iostream>
int main() {
    std::cout << "orders_functional placeholder\n";
    return 0;
}
```

- [x] **Step 4: Configure and build (red→green: must compile and link)**

Run:
```bash
cmake -S "${SAMPLE_DIR}" -B "${SAMPLE_DIR}/build" \
  -DCMAKE_PREFIX_PATH="${HALCYON_BUILD}" \
  -DDB2_CLIDRIVER_ROOT="${DRIVER_ROOT}"
cmake --build "${SAMPLE_DIR}/build" -j
```
Expected: configures (finds Halcyon + DB2CLI), builds `orders_oo` and
`orders_functional` with no errors.

- [x] **Step 5: Run the connectivity smoke against live Db2**

Run: `"${SAMPLE_DIR}/build/orders_oo"`
Expected: `connected, value=1` and exit 0. (Db2 from Task 2 must be up.)

- [x] **Step 6: Commit**

```bash
git add samples/orders/CMakeLists.txt samples/orders/src/orders_oo.cpp samples/orders/src/orders_functional.cpp
git commit -m "feat: scaffold orders sample CMake project with connectivity smoke"
```

---

## Task 4: Shared model header

**Files:**
- Create: `samples/orders/src/orders_model.hpp`

- [x] **Step 1: Write `samples/orders/src/orders_model.hpp`**

```cpp
#pragma once

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>

#include <halcyon/halcyon.hpp>

// --- Reflected row types ---------------------------------------------------

struct Customer {
    int                        customer_id = 0;
    std::string                name;
    std::optional<std::string> email;        // maps the nullable EMAIL column
    halcyon::Date              created_at;
};
HALCYON_REFLECT(Customer, customer_id, name, email, created_at);

struct Order {
    std::string        order_no;
    int                customer_id = 0;
    std::string        status;
    halcyon::Decimal   total_amount;
    halcyon::Timestamp order_ts;
};
HALCYON_REFLECT(Order, order_no, customer_id, status, total_amount, order_ts);

// --- SQL -------------------------------------------------------------------
// IMPORTANT: column order in every SELECT must match the struct field order
// above, because queryAs<T> maps columns to fields positionally.

namespace sql {
// Demo rows this sample inserts; deleted up front so each run is idempotent.
inline constexpr const char* kResetDemoRows =
    "DELETE FROM orders WHERE order_no IN ('ORD-2001','ORD-2002','ORD-2003')";
inline constexpr const char* kSelectCustomers =
    "SELECT customer_id, name, email, created_at "
    "FROM customers ORDER BY customer_id";
inline constexpr const char* kSelectOrdersForCustomer =
    "SELECT order_no, customer_id, status, total_amount, order_ts "
    "FROM orders WHERE customer_id = :cid ORDER BY order_no";  // named param
inline constexpr const char* kInsertOrder =
    "INSERT INTO orders(order_no, customer_id, status, total_amount, order_ts) "
    "VALUES (?, ?, ?, ?, ?)";                                  // positional
inline constexpr const char* kUpdateOrderStatus =
    "UPDATE orders SET status = ? WHERE order_no = ?";
inline constexpr const char* kSelectOrderByNo =
    "SELECT order_no, customer_id, status, total_amount, order_ts "
    "FROM orders WHERE order_no = ?";
}  // namespace sql

// --- Shared helpers --------------------------------------------------------

inline std::string resolveDsn() {
    if (const char* env = std::getenv("HALCYON_TEST_DSN")) return env;
    return "DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;";
}

inline void printCustomer(const Customer& c) {
    std::cout << "  #" << c.customer_id << "  " << c.name << "  <"
              << (c.email ? *c.email : std::string("(no email)")) << ">"
              << "  joined " << c.created_at.value << "\n";
}

inline void printOrder(const Order& o) {
    std::cout << "  " << o.order_no << "  cust#" << o.customer_id << "  "
              << o.status << "  $" << o.total_amount.str() << "  "
              << o.order_ts.value << "\n";
}
```

- [x] **Step 2: Verify it compiles by including it from the smoke binary**

Temporarily add `#include "orders_model.hpp"` at the top of the current
`orders_oo.cpp` smoke file, then rebuild:

Run: `cmake --build "${SAMPLE_DIR}/build" --target orders_oo -j`
Expected: compiles with no errors (validates the header + `HALCYON_REFLECT`
expansions). Remove the temporary include afterward — Task 5 rewrites the file.

- [x] **Step 3: Commit**

```bash
git add samples/orders/src/orders_model.hpp
git commit -m "feat: add shared orders sample model header"
```

---

## Task 5: OO / throwing-style walkthrough

**Files:**
- Modify (replace): `samples/orders/src/orders_oo.cpp`

- [x] **Step 1: Replace `samples/orders/src/orders_oo.cpp` with the full walkthrough**

```cpp
// Halcyon orders sample — object-oriented / throwing style.
// Runs the SAME seven steps as orders_functional.cpp; diff the two files to see
// how the throwing API compares to the Result<T> API.
#include <cstdint>
#include <iostream>
#include <string>
#include <tuple>

#include "orders_model.hpp"

int main() {
    const std::string dsn = resolveDsn();
    try {
        // 1. Open the database.
        auto db = halcyon::Database::openOrThrow(dsn);
        std::cout << "== Connected ==\n";

        // Make the run idempotent: drop any rows a previous run inserted.
        db.executeOrThrow(sql::kResetDemoRows);

        // 2. Query all customers, mapped into a typed struct vector.
        std::cout << "\n== Customers ==\n";
        auto customers = db.queryAsOrThrow<Customer>(sql::kSelectCustomers);
        for (const auto& c : customers) printCustomer(c);

        // 3. Query one customer's orders using a NAMED parameter.
        std::cout << "\n== Orders for customer #1 ==\n";
        auto orders = db.queryAsOrThrow<Order>(
            sql::kSelectOrdersForCustomer, halcyon::params{{":cid", 1}});
        for (const auto& o : orders) printOrder(o);

        // 4. Insert one order with POSITIONAL binds (Decimal + Timestamp).
        auto inserted = db.executeOrThrow(
            sql::kInsertOrder, std::string("ORD-2001"), 1, std::string("NEW"),
            halcyon::Decimal("129.99"),
            halcyon::Timestamp{"2026-06-22 09:30:00"});
        std::cout << "\n== Inserted " << inserted << " order ==\n";

        // 5. Batch-insert two more orders (tuple rows).
        auto batch = halcyon::batchOf({
            std::make_tuple(std::string("ORD-2002"), 2, std::string("NEW"),
                            halcyon::Decimal("59.90"),
                            halcyon::Timestamp{"2026-06-22 09:35:00"}),
            std::make_tuple(std::string("ORD-2003"), 3, std::string("NEW"),
                            halcyon::Decimal("249.00"),
                            halcyon::Timestamp{"2026-06-22 09:40:00"}),
        });
        auto batched = db.executeBatch(sql::kInsertOrder, batch).value();
        std::cout << "== Batch inserted " << batched << " orders ==\n";

        // 6. Update two orders atomically inside a transaction. Returning an
        //    error Result from the lambda would roll back instead of commit.
        auto updated = db.transaction(
            [](halcyon::Transaction& tx) -> halcyon::Result<std::int64_t> {
                auto a = tx.execute(sql::kUpdateOrderStatus,
                                    std::string("SHIPPED"),
                                    std::string("ORD-2001"));
                if (!a.ok()) return a.error();
                auto b = tx.execute(sql::kUpdateOrderStatus,
                                    std::string("PAID"),
                                    std::string("ORD-2002"));
                if (!b.ok()) return b.error();
                return a.value() + b.value();
            });
        std::cout << "== Transaction updated " << updated.value()
                  << " orders ==\n";

        // 7. Re-query to prove the update landed (read-after-write).
        std::cout << "\n== ORD-2001 after update ==\n";
        auto after = db.queryAsOrThrow<Order>(sql::kSelectOrderByNo,
                                              std::string("ORD-2001"));
        for (const auto& o : after) printOrder(o);

        std::cout << "\n== Done ==\n";
    } catch (const halcyon::Exception& e) {
        std::cerr << "halcyon error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
```

- [x] **Step 2: Build (red→green: must compile)**

Run: `cmake --build "${SAMPLE_DIR}/build" --target orders_oo -j`
Expected: compiles and links with no errors.

- [x] **Step 3: Run against live Db2 and verify output**

Run: `"${SAMPLE_DIR}/build/orders_oo"`
Expected: exit 0; output shows `== Connected ==`, three customers (Linus shows
`<(no email)>`), customer #1's seed orders, `Inserted 1 order`, `Batch inserted
2 orders`, `Transaction updated 2 orders`, and `ORD-2001` with status `SHIPPED`.
The `order_ts` prints in Db2's timestamp text form (e.g.
`2026-06-22-09.30.00.000000`).

- [x] **Step 4: Verify rerunnability**

Run: `"${SAMPLE_DIR}/build/orders_oo"` a second time.
Expected: identical output, exit 0 (the `kResetDemoRows` delete makes the
inserts/updates repeatable; no duplicate-key error).

- [x] **Step 5: Commit**

```bash
git add samples/orders/src/orders_oo.cpp
git commit -m "feat: implement OO-style orders sample walkthrough"
```

---

## Task 6: Functional / Result-style walkthrough

**Files:**
- Modify (replace): `samples/orders/src/orders_functional.cpp`

- [x] **Step 1: Replace `samples/orders/src/orders_functional.cpp` with the full walkthrough**

```cpp
// Halcyon orders sample — functional / Result<T> style.
// Runs the SAME seven steps as orders_oo.cpp using the free-function API and
// explicit .ok()/.error() checks (no exceptions).
#include <cstdint>
#include <iostream>
#include <string>
#include <tuple>

#include "orders_model.hpp"

namespace {
// Print an error and return a non-zero exit code.
int fail(const halcyon::Error& e) {
    std::cerr << "halcyon error: " << e.message
              << " (sqlstate=" << e.sqlstate << ")\n";
    return 1;
}
}  // namespace

int main() {
    const std::string dsn = resolveDsn();

    // 1. Open the database.
    auto dbr = halcyon::Database::open(dsn);
    if (!dbr.ok()) return fail(dbr.error());
    halcyon::Database db = std::move(dbr.value());
    std::cout << "== Connected ==\n";

    // Make the run idempotent.
    if (auto r = halcyon::execute(db, sql::kResetDemoRows); !r.ok())
        return fail(r.error());

    // 2. Query all customers into a typed struct vector.
    std::cout << "\n== Customers ==\n";
    auto customers = halcyon::query_as<Customer>(db, sql::kSelectCustomers);
    if (!customers.ok()) return fail(customers.error());
    for (const auto& c : customers.value()) printCustomer(c);

    // 3. Query one customer's orders using a NAMED parameter.
    std::cout << "\n== Orders for customer #1 ==\n";
    auto orders = halcyon::query_as<Order>(db, sql::kSelectOrdersForCustomer,
                                           halcyon::params{{":cid", 1}});
    if (!orders.ok()) return fail(orders.error());
    for (const auto& o : orders.value()) printOrder(o);

    // 4. Insert one order with POSITIONAL binds (Decimal + Timestamp).
    auto inserted = halcyon::execute(
        db, sql::kInsertOrder, std::string("ORD-2001"), 1, std::string("NEW"),
        halcyon::Decimal("129.99"), halcyon::Timestamp{"2026-06-22 09:30:00"});
    if (!inserted.ok()) return fail(inserted.error());
    std::cout << "\n== Inserted " << inserted.value() << " order ==\n";

    // 5. Batch-insert two more orders (tuple rows).
    auto batch = halcyon::batchOf({
        std::make_tuple(std::string("ORD-2002"), 2, std::string("NEW"),
                        halcyon::Decimal("59.90"),
                        halcyon::Timestamp{"2026-06-22 09:35:00"}),
        std::make_tuple(std::string("ORD-2003"), 3, std::string("NEW"),
                        halcyon::Decimal("249.00"),
                        halcyon::Timestamp{"2026-06-22 09:40:00"}),
    });
    auto batched = db.executeBatch(sql::kInsertOrder, batch);
    if (!batched.ok()) return fail(batched.error());
    std::cout << "== Batch inserted " << batched.value() << " orders ==\n";

    // 6. Update two orders atomically inside a transaction. Returning an error
    //    Result from the lambda rolls back instead of committing.
    auto updated = halcyon::transaction(
        db, [](halcyon::Transaction& tx) -> halcyon::Result<std::int64_t> {
            auto a = tx.execute(sql::kUpdateOrderStatus, std::string("SHIPPED"),
                                std::string("ORD-2001"));
            if (!a.ok()) return a.error();
            auto b = tx.execute(sql::kUpdateOrderStatus, std::string("PAID"),
                                std::string("ORD-2002"));
            if (!b.ok()) return b.error();
            return a.value() + b.value();
        });
    if (!updated.ok()) return fail(updated.error());
    std::cout << "== Transaction updated " << updated.value() << " orders ==\n";

    // 7. Re-query to prove the update landed (read-after-write).
    std::cout << "\n== ORD-2001 after update ==\n";
    auto after = halcyon::query_as<Order>(db, sql::kSelectOrderByNo,
                                          std::string("ORD-2001"));
    if (!after.ok()) return fail(after.error());
    for (const auto& o : after.value()) printOrder(o);

    std::cout << "\n== Done ==\n";
    return 0;
}
```

- [x] **Step 2: Build (red→green: must compile)**

Run: `cmake --build "${SAMPLE_DIR}/build" --target orders_functional -j`
Expected: compiles and links with no errors.

- [x] **Step 3: Run and verify identical output to the OO binary**

Run:
```bash
"${SAMPLE_DIR}/build/orders_functional" > /tmp/func.out
"${SAMPLE_DIR}/build/orders_oo"         > /tmp/oo.out
diff /tmp/oo.out /tmp/func.out && echo "IDENTICAL"
```
Expected: `diff` prints nothing and then `IDENTICAL` (both binaries produce the
same transcript); each exits 0.

- [x] **Step 4: Commit**

```bash
git add samples/orders/src/orders_functional.cpp
git commit -m "feat: implement functional-style orders sample walkthrough"
```

---

## Task 7: README setup procedure

**Files:**
- Create: `samples/orders/README.md`

- [x] **Step 1: Write `samples/orders/README.md`**

````markdown
# Halcyon Orders Sample

A self-contained sample showing how to consume Halcyon from a downstream
project and perform **query / insert / update** against a local IBM Db2, in two
parallel styles that implement identical logic:

- `orders_oo` — object-oriented / throwing API (`openOrThrow`, `queryAsOrThrow`, …)
- `orders_functional` — functional / `Result<T>` API (`halcyon::query_as`, …)

Both run the same seven steps over a mini order system (`customers` 1:N
`orders`): connect → query customers (typed struct mapping) → query orders by a
named parameter → insert an order → batch-insert orders → update orders inside a
transaction → re-query. Shared row structs and SQL live in
`src/orders_model.hpp`.

## Prerequisites

- Docker (the repo's `docker/docker-compose.yml` provides Db2 `SAMPLE`).
- A built Halcyon tree or install prefix.
- The vendored CLI driver at `third_party/clidriver` (see the repo `AGENTS.md`;
  on macOS run the one-time `xattr -r -d com.apple.quarantine third_party/clidriver`).

## 1. Start Db2

```bash
docker compose -f ../../docker/docker-compose.yml up -d
docker compose -f ../../docker/docker-compose.yml ps   # wait for STATUS = healthy
```

The first boot creates `SAMPLE` (~2 min native; longer under amd64 emulation).

## 2. Load the schema and seed data

```bash
./load_sql.sh
```

This copies `sql/schema.sql` and `sql/seed.sql` into the container and applies
them with `db2 -tvf`. On a fresh database the `DROP` lines print `SQL0204N`
(harmless). The raw equivalent, if you prefer to run it by hand:

```bash
docker compose -f ../../docker/docker-compose.yml cp sql/schema.sql db2:/tmp/schema.sql
docker compose -f ../../docker/docker-compose.yml exec db2 \
  su - db2inst1 -c "db2 connect to SAMPLE && db2 -tvf /tmp/schema.sql"
# …repeat for seed.sql
```

Apple `container` users: there is no Compose; use `container cp` and
`container exec halcyon-db2 bash -lc 'su - db2inst1 -c ". ~/sqllib/db2profile; db2 connect to SAMPLE && db2 -tvf /tmp/schema.sql"'`
(see `docker/README.md`). Note: Db2's image is amd64-only and does not start
under Apple `container` on Apple silicon — use Docker there.

## 3. Build the sample

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=<halcyon-build-or-install-tree> \
  -DDB2_CLIDRIVER_ROOT=<halcyon-repo>/third_party/clidriver
cmake --build build -j
```

`find_package(Halcyon)` needs `CMAKE_PREFIX_PATH` to point at a Halcyon build
tree or install prefix. `DB2_CLIDRIVER_ROOT` must point at Halcyon's vendored
driver, because the driver's default search root is relative to *this* project.
Halcyon adds the driver lib dir to the RPATH, so no `DYLD_LIBRARY_PATH` /
`LD_LIBRARY_PATH` is needed at run time.

## 4. Run

```bash
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
./build/orders_oo
./build/orders_functional
```

If `HALCYON_TEST_DSN` is unset, both binaries fall back to the same localhost
DSN shown above.

### Expected output (identical for both binaries)

```
== Connected ==

== Customers ==
  #1  Ada Lovelace  <ada@example.com>  joined 2026-01-15
  #2  Linus Torvalds  <(no email)>  joined 2026-02-03
  #3  Grace Hopper  <grace@example.com>  joined 2026-03-21

== Orders for customer #1 ==
  ORD-1001  cust#1  PAID  $199.99  2026-04-01-10.00.00.000000
  ORD-1002  cust#1  NEW  $49.50  2026-04-02-11.30.00.000000

== Inserted 1 order ==
== Batch inserted 2 orders ==
== Transaction updated 2 orders ==

== ORD-2001 after update ==
  ORD-2001  cust#1  SHIPPED  $129.99  2026-06-22-09.30.00.000000

== Done ==
```

Dates and timestamps print in Db2's text form (`DATE` as `YYYY-MM-DD`,
`TIMESTAMP` as `YYYY-MM-DD-HH.MM.SS.ffffff`). The sample deletes its demo rows
(`ORD-2001`..`ORD-2003`) on startup, so it is safe to run repeatedly.

## 5. Tear down

```bash
docker compose -f ../../docker/docker-compose.yml down
```
````

- [x] **Step 2: Sanity-check the documented commands against the live run**

Confirm the expected-output block matches what Tasks 5–6 actually printed
(customer rows, counts, final `ORD-2001 SHIPPED $129.99`). Adjust the README's
transcript if Db2's timestamp/decimal text differs on your build, then re-verify.

- [x] **Step 3: Commit**

```bash
git add samples/orders/README.md
git commit -m "docs: add orders sample README with setup procedure"
```

---

## Task 8: Link the sample from the repo README

**Files:**
- Modify: `README.md:205`

- [x] **Step 1: Add a `samples/` line to the repository layout block**

In `README.md`, the layout block currently has (around line 205):

```
examples/                 OO and functional usage samples
docker/                   Db2 Compose for integration tests
```

Change it to:

```
examples/                 OO and functional usage samples
samples/orders/           Standalone consumer sample (find_package + local Db2)
docker/                   Db2 Compose for integration tests
```

- [x] **Step 2: Verify the edit**

Run: `git diff README.md`
Expected: the single added `samples/orders/` line in the layout block.

- [x] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: reference orders sample in repo layout"
```

---

## Task 9: Full end-to-end verification

This is the AGENTS.md verification gate: a clean, live run of the whole flow.

- [x] **Step 1: Clean rebuild of the sample**

Run:
```bash
rm -rf "${SAMPLE_DIR}/build"
cmake -S "${SAMPLE_DIR}" -B "${SAMPLE_DIR}/build" \
  -DCMAKE_PREFIX_PATH="${HALCYON_BUILD}" \
  -DDB2_CLIDRIVER_ROOT="${DRIVER_ROOT}"
cmake --build "${SAMPLE_DIR}/build" -j
```
Expected: clean configure + build, no warnings-as-errors failures.

- [x] **Step 2: Fresh schema/seed load**

Run:
```bash
docker compose -f docker/docker-compose.yml up -d
docker compose -f docker/docker-compose.yml ps   # healthy
./samples/orders/load_sql.sh
```
Expected: `done`; `customers`=3, `orders`=4 (re-verify with the Task 2 Step 4
count query if desired).

- [x] **Step 3: Run both binaries and confirm identical, correct output**

Run:
```bash
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
"${SAMPLE_DIR}/build/orders_oo"         | tee /tmp/oo.out
"${SAMPLE_DIR}/build/orders_functional" | tee /tmp/func.out
diff /tmp/oo.out /tmp/func.out && echo "IDENTICAL"
```
Expected: both exit 0, transcript matches the README's expected output, and
`diff` reports `IDENTICAL`.

- [x] **Step 4: Confirm rerunnability and tear down**

Run:
```bash
"${SAMPLE_DIR}/build/orders_oo" >/dev/null && echo "RERUN OK"
docker compose -f docker/docker-compose.yml down
```
Expected: `RERUN OK` (second run succeeds with no duplicate-key error), then the
container stops.

- [x] **Step 5: Final commit (if any README transcript tweaks were needed)**

```bash
git add -A samples/orders
git commit -m "test: verify orders sample end-to-end against live Db2"
```

(Skip if there is nothing to commit.)

---

## Self-review notes

- **Spec coverage:** standalone `samples/orders/` consumer (Task 3) ✓; mini
  order schema + seed (Task 1) ✓; `find_package(Halcyon)` + `DB2_CLIDRIVER_ROOT`
  (Task 3, README) ✓; shared model with `HALCYON_REFLECT` + SQL (Task 4) ✓;
  named params, positional insert, batch, transaction, Decimal/Date/Timestamp
  (Tasks 5–6) ✓; OO and functional parity (Tasks 5–6, diff in Task 6/9) ✓;
  documented setup procedure (Task 7) ✓; live-Db2 verification (Task 9) ✓;
  rerunnability via demo-row reset (Tasks 5, 9) ✓; discoverability (Task 8) ✓.
- **Type/name consistency:** `Customer`/`Order` fields and `sql::k*` constant
  names are defined once in Task 4 and referenced verbatim in Tasks 5–6; the
  transaction lambda returns `halcyon::Result<std::int64_t>` in both files;
  order numbers `ORD-2001..2003` match `kResetDemoRows`.
- **Note:** Task 3 intentionally ships a *temporary* minimal `orders_oo.cpp`
  (replaced in Task 5) plus a functional stub (replaced in Task 6) so the
  two-target CMake configures and a connectivity smoke runs before the full
  walkthroughs exist. Use the corrected minimal body called out in Task 3 Step 2.
```
