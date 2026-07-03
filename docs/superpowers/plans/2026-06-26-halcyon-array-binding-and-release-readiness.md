# True Db2 CLI Array Binding + v1 Release-Readiness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace per-row `executeBatch` with true Db2 CLI column-wise array binding (honoring the v1 spec promise), then run the v1 release-readiness pass: docs/plan reconciliation, full live Db2 integration, TSan stress, orders sample E2E, and CI parity.

**Architecture:** Array binding lives below the `ICliDriver` seam as one new combined bind+execute method; the portable layer validates the batch (rectangular + type-homogeneous) and delegates. The Db2 driver binds each column as a contiguous array, sets `SQL_ATTR_PARAMSET_SIZE`, executes once per byte-budget chunk, and aggregates affected counts. Public API is unchanged.

**Tech Stack:** C++17, IBM Db2 CLI (`sqlcli1.h`), CMake, GoogleTest, Docker (Db2 integration), ThreadSanitizer.

**Spec:** `docs/superpowers/specs/2026-06-26-halcyon-array-binding-design.md`

---

## File structure

| File | Responsibility | Change |
|------|----------------|--------|
| `include/halcyon/detail/cli/driver.hpp` | Seam interface | Add `executeBatch(stmt, rows)` pure virtual |
| `include/halcyon/detail/batch_validate.hpp` | Portable batch validation (NEW) | `validate_batch_rows()` free function |
| `include/halcyon/connection.hpp` | Portable batch entry | Validate + delegate to seam (drop per-row loop) |
| `src/detail/cli/db2_cli_driver.cpp` | Db2 driver | Implement `executeBatch` (provisional → array binding) |
| `tests/unit/mock_cli_driver.hpp` | Test double | Implement `executeBatch` (record + scriptable) |
| `tests/unit/test_database_batch.cpp` | Unit tests | Rewrite for the seam-delegation contract |
| `tests/integration/test_db2_roundtrip.cpp` | Live tests | Array-binding round-trips |
| `tests/stress/perf/batch_insert_bench.cpp` | Live benchmark (NEW) | per-row vs array vs txn-wrapped throughput |
| `tests/stress/perf/CMakeLists.txt` | Build wiring | Add the benchmark target |
| `docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md` | Parent spec §5 | Describe array binding + failure contract |
| `docs/guide/batch-operations.md`, `README.md` | Docs | Replace per-row wording |
| `docs/superpowers/plans/2026-06-22-halcyon-orders-sample.md` | Stale plan | Check completed boxes |
| `docs/superpowers/plans/2026-06-24-halcyon-otel-context-propagation.md` | Stale plan | Check completed boxes |
| `.github/workflows/ci.yml` | CI | Align with local verification steps |

---

## Task 1: Seam `executeBatch` + portable validation/delegation (behavior-preserving)

Introduces the new seam method and moves batch orchestration above the seam, **without** changing observable behavior yet — the Db2 driver keeps executing per row internally. All coverage is unit-level against `MockCliDriver`.

**Files:**
- Modify: `include/halcyon/detail/cli/driver.hpp`
- Create: `include/halcyon/detail/batch_validate.hpp`
- Modify: `include/halcyon/connection.hpp:372`
- Modify: `include/halcyon/transaction.hpp` (add `Transaction::executeBatch`)
- Modify: `include/halcyon/database.hpp` (add `ScopedTransaction::executeBatch`)
- Modify: `src/detail/cli/db2_cli_driver.cpp` (add provisional `executeBatch`)
- Modify: `tests/unit/mock_cli_driver.hpp`
- Test: `tests/unit/test_database_batch.cpp`

- [ ] **Step 1: Rewrite the unit tests (the failing spec)**

Replace the entire contents of `tests/unit/test_database_batch.cpp` with:

```cpp
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "halcyon/database.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;
using Value = halcyon::detail::cli::Value;
using halcyon::detail::cli::Null;

namespace {
PoolConfig noThread() {
    PoolConfig c;
    c.startMaintenanceThread = false;
    return c;
}
}  // namespace

struct Pair {
    std::int64_t a;
    std::string b;
};
HALCYON_REFLECT(Pair, a, b);

TEST(Batch, DelegatesToDriverReturningTotal) {
    MockCliDriver driver;
    driver.batchRowCounts.push_back(3);
    auto db = Database::open(driver, "X", noThread()).value();

    auto batch = halcyon::batchOf<Pair>({{1, "a"}, {2, "b"}, {3, "c"}});
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 3);
    EXPECT_EQ(driver.executeBatchCalls, 1);      // one array call, not per row
    EXPECT_EQ(driver.preparedSql.size(), 1u);    // single prepare
    ASSERT_EQ(driver.lastBatchRows.size(), 3u);  // all rows handed to the driver
    EXPECT_EQ(driver.lastBatchRows[0].size(), 2u);
}

TEST(Batch, EmptyBatchSkipsDriver) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", halcyon::Batch{});
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 0);
    EXPECT_EQ(driver.executeBatchCalls, 0);
    EXPECT_EQ(driver.preparedSql.size(), 0u);
}

TEST(Batch, DriverErrorPropagates) {
    MockCliDriver driver;
    halcyon::Error e;
    e.code = halcyon::ErrorCode::Constraint;
    e.message = "dup";
    driver.batchErrors.push_back(e);
    auto db = Database::open(driver, "X", noThread()).value();

    auto batch = halcyon::batchOf<Pair>({{1, "a"}, {2, "b"}});
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, halcyon::ErrorCode::Constraint);
}

TEST(Batch, ConnectionErrorDiscardsConnection) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    auto db = Database::open(driver, "X", cfg).value();
    ASSERT_EQ(driver.connectCalls, 1);

    halcyon::Error ce;
    ce.code = halcyon::ErrorCode::Connection;
    ce.message = "dead";
    driver.batchErrors.push_back(ce);

    auto batch = halcyon::batchOf<Pair>({{1, "a"}, {2, "b"}});
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, halcyon::ErrorCode::Connection);
    EXPECT_EQ(driver.disconnectCalls, 1);  // broken connection discarded
}

TEST(Batch, RaggedRowsRejectedBeforeDriver) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();

    halcyon::Batch b;
    b.rows = {{Value{std::int64_t{1}}, Value{std::string{"a"}}},
              {Value{std::int64_t{2}}}};  // 2 cols then 1 col
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", b);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, halcyon::ErrorCode::Mapping);
    EXPECT_EQ(driver.executeBatchCalls, 0);
    EXPECT_EQ(driver.preparedSql.size(), 0u);
}

TEST(Batch, MixedTypeColumnRejectedBeforeDriver) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();

    halcyon::Batch b;
    b.rows = {{Value{std::int64_t{1}}},
              {Value{std::string{"x"}}}};  // column 0: int64 then string
    auto n = db.executeBatch("INSERT INTO t(a) VALUES (?)", b);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, halcyon::ErrorCode::Mapping);
    EXPECT_EQ(driver.executeBatchCalls, 0);
}

TEST(Batch, NullsInColumnPassThrough) {
    MockCliDriver driver;
    driver.batchRowCounts.push_back(2);
    auto db = Database::open(driver, "X", noThread()).value();

    halcyon::Batch b;
    b.rows = {{Value{std::int64_t{1}}, Value{Null{}}},
              {Value{std::int64_t{2}}, Value{std::string{"b"}}}};
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", b);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 2);
    ASSERT_EQ(driver.lastBatchRows.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<Null>(driver.lastBatchRows[0][1]));
}

TEST(Batch, AllNullColumnIsValid) {
    MockCliDriver driver;
    driver.batchRowCounts.push_back(2);
    auto db = Database::open(driver, "X", noThread()).value();

    halcyon::Batch b;
    b.rows = {{Value{std::int64_t{1}}, Value{Null{}}},
              {Value{std::int64_t{2}}, Value{Null{}}}};  // column 1 all-NULL
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", b);
    ASSERT_TRUE(n.ok());  // all-NULL column carries no type conflict
    EXPECT_EQ(n.value(), 2);
    EXPECT_EQ(driver.executeBatchCalls, 1);
}

TEST(Batch, TupleRowsBuildPositionalBinds) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();

    auto batch = halcyon::batchOf({
        std::make_tuple(std::int64_t{1}, std::string{"a"}),
        std::make_tuple(std::int64_t{2}, std::string{"b"}),
    });
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 2);  // mock default return == rows.size()
    EXPECT_EQ(driver.preparedSql.size(), 1u);
    EXPECT_EQ(driver.executeBatchCalls, 1);
}

TEST(Batch, ExecutesInsideTransaction) {
    MockCliDriver driver;
    driver.batchRowCounts.push_back(2);
    auto db = Database::open(driver, "X", noThread()).value();

    auto tx = db.begin().value();
    auto batch = halcyon::batchOf<Pair>({{1, "a"}, {2, "b"}});
    auto n = tx.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 2);
    ASSERT_TRUE(tx.commit().ok());
    EXPECT_EQ(driver.executeBatchCalls, 1);
    EXPECT_EQ(driver.commitCalls, 1);
    // autocommit toggled OFF for the unit of work, then back ON on commit.
    ASSERT_GE(driver.autoCommitCalls.size(), 2u);
    EXPECT_FALSE(driver.autoCommitCalls.front());
    EXPECT_TRUE(driver.autoCommitCalls.back());
}
```

- [ ] **Step 2: Run the tests to verify they fail to compile**

Run: `cmake --build build -j 2>&1 | head -30`
Expected: compile error — `MockCliDriver` has no `executeBatch`/`batchRowCounts`, and `ICliDriver` is missing the pure virtual.

- [ ] **Step 3: Add the seam method**

In `include/halcyon/detail/cli/driver.hpp`, after the `bindParams` declaration (line ~52), add:

```cpp
    // Binds `rows` as a column-wise parameter array and executes via array
    // binding, chunking internally to bound memory. Returns total rows affected,
    // summed across chunks. Precondition: `rows` is non-empty, rectangular, and
    // each column is type-homogeneous (validated above the seam). On any row
    // failure, returns a single Error naming the first failing row index.
    virtual Result<std::int64_t> executeBatch(
        StatementHandle stmt,
        const std::vector<std::vector<Value>>& rows) = 0;
```

- [ ] **Step 4: Create the portable validation helper**

Create `include/halcyon/detail/batch_validate.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/error.hpp"
#include "halcyon/result.hpp"

namespace halcyon::detail {

// Validates that a batch is rectangular and each column is type-homogeneous,
// ignoring NULLs. Portable (operates on cli::Value variants, no CLI types), so
// it is unit-testable against MockCliDriver. Caller guarantees `rows` non-empty.
inline Result<void> validate_batch_rows(
    const std::vector<std::vector<cli::Value>>& rows) {
    constexpr std::size_t kUnset = static_cast<std::size_t>(-1);
    const std::size_t ncols = rows.front().size();
    std::vector<std::size_t> colType(ncols, kUnset);  // variant index per column
    for (std::size_t r = 0; r < rows.size(); ++r) {
        if (rows[r].size() != ncols) {
            Error e;
            e.code = ErrorCode::Mapping;
            e.message = "batch row " + std::to_string(r) + " has " +
                        std::to_string(rows[r].size()) +
                        " columns, expected " + std::to_string(ncols);
            return e;
        }
        for (std::size_t c = 0; c < ncols; ++c) {
            const auto& v = rows[r][c];
            if (std::holds_alternative<cli::Null>(v)) continue;  // null: any type
            const std::size_t idx = v.index();
            if (colType[c] == kUnset) {
                colType[c] = idx;
            } else if (colType[c] != idx) {
                Error e;
                e.code = ErrorCode::Mapping;
                e.message = "batch column " + std::to_string(c) +
                            " mixes value types (first at row " +
                            std::to_string(r) + ")";
                return e;
            }
        }
    }
    return Result<void>();
}

}  // namespace halcyon::detail
```

- [ ] **Step 5: Rewire `Connection::executeBatch` and add transaction batch**

In `include/halcyon/connection.hpp`, add near the other detail includes at the top:

```cpp
#include "halcyon/detail/batch_validate.hpp"
```

Replace the existing `executeBatch` method body (connection.hpp:372–393) with:

```cpp
    // Validates the batch (rectangular + type-homogeneous) then delegates to the
    // driver's array-binding executeBatch. Returns total rows affected.
    Result<std::int64_t> executeBatch(
        const std::string& sql,
        const std::vector<std::vector<detail::cli::Value>>& rows) {
        if (rows.empty()) return std::int64_t{0};
        if (auto v = detail::validate_batch_rows(rows); !v.ok())
            return v.error();
        auto lease = cache_->acquire(sql);
        if (!lease.ok()) return lease.error();
        auto e = driver_->executeBatch(lease.value().handle(), rows);
        if (!e.ok()) {
            lease.value().poison();
            return e.error();
        }
        return e.value();
    }
```

`Transaction` currently has no `executeBatch`, so the spec's "batch inside a
transaction" atomicity pattern is not yet expressible. Add it. In
`include/halcyon/transaction.hpp`, alongside the `execute`/`query` forwarders
(after the `query(..., params)` overload, ~line 62), add:

```cpp
    // Batch insert within the unit of work; forwards to the leased connection.
    Result<std::int64_t> executeBatch(
        const std::string& sql,
        const std::vector<std::vector<detail::cli::Value>>& rows) {
        return conn_->executeBatch(sql, rows);
    }
```

In `include/halcyon/database.hpp`, add the ergonomic `Batch` overload to
`ScopedTransaction` next to its other forwarders (after `queryAs`, ~line 195;
`Batch` is defined earlier in this header so it is visible):

```cpp
    Result<std::int64_t> executeBatch(const std::string& sql,
                                      const Batch& batch) {
        return txn_.executeBatch(sql, batch.rows);
    }
```

- [ ] **Step 6: Add the provisional Db2 driver implementation**

In `src/detail/cli/db2_cli_driver.cpp`, immediately after the `execute` method (ends line ~226), add a behavior-preserving implementation that still loops per row internally (true array binding lands in Task 2):

```cpp
    Result<std::int64_t> executeBatch(
        StatementHandle stmt,
        const std::vector<std::vector<Value>>& rows) override {
        // Provisional: per-row execution (Task 1). Task 2 replaces this body
        // with true column-wise array binding. Behavior is identical for now.
        if (!stmt_state(stmt)) return unknown_stmt();
        std::int64_t total = 0;
        for (const auto& row : rows) {
            auto b = bindParams(stmt, row);
            if (!b.ok()) return b.error();
            auto e = execute(stmt);
            if (!e.ok()) return e.error();
            total += e.value();
        }
        return total;
    }
```

- [ ] **Step 7: Implement `executeBatch` in the mock**

In `tests/unit/mock_cli_driver.hpp`, add these fields after the `execRowCounts` declaration (line ~49):

```cpp
    // --- batch scripting (array binding) ---
    std::deque<std::int64_t> batchRowCounts;  // executeBatch() returns the next
    std::deque<Error> batchErrors;            // executeBatch() fails with the next
    int executeBatchCalls = 0;
    std::vector<std::vector<Value>> lastBatchRows;  // rows from the last call
```

Add this method after `bindParams` (line ~110):

```cpp
    Result<std::int64_t> executeBatch(
        StatementHandle stmt,
        const std::vector<std::vector<Value>>& rows) override {
        (void)stmt;
        ++executeBatchCalls;
        lastBatchRows = rows;
        if (!batchErrors.empty()) {
            Error e = batchErrors.front();
            batchErrors.pop_front();
            return Result<std::int64_t>(e);
        }
        if (!batchRowCounts.empty()) {
            std::int64_t n = batchRowCounts.front();
            batchRowCounts.pop_front();
            return Result<std::int64_t>(n);
        }
        return Result<std::int64_t>(static_cast<std::int64_t>(rows.size()));
    }
```

- [ ] **Step 8: Build and run the unit tests**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R Batch`
Expected: all `Batch.*` tests PASS.

- [ ] **Step 9: Run the full unit suite for regressions**

Run: `ctest --test-dir build --output-on-failure`
Expected: all tests PASS (no behavior change elsewhere).

- [ ] **Step 10: Commit**

```bash
git add include/halcyon/detail/cli/driver.hpp include/halcyon/detail/batch_validate.hpp \
        include/halcyon/connection.hpp include/halcyon/transaction.hpp \
        include/halcyon/database.hpp src/detail/cli/db2_cli_driver.cpp \
        tests/unit/mock_cli_driver.hpp tests/unit/test_database_batch.cpp
git commit -m "feat: route executeBatch through a single seam method with portable validation"
```

---

## Task 2: True Db2 CLI array binding in the driver

Replaces the provisional per-row loop with real column-wise array binding. Verified by live integration tests (opt-in; needs `HALCYON_TEST_DSN`).

**Files:**
- Modify: `src/detail/cli/db2_cli_driver.cpp`
- Test: `tests/integration/test_db2_roundtrip.cpp`

- [ ] **Step 1: Add the failing integration tests**

Append to `tests/integration/test_db2_roundtrip.cpp` (before the final `}` if file-scoped; these are free `TEST`s so just append at end):

```cpp
// --- True array binding (Task 2) ---

namespace {
struct ArrRow {
    std::int64_t id;
    std::string name;
    std::optional<std::int64_t> qty;  // nullable column
};
}  // namespace
HALCYON_REFLECT(ArrRow, id, name, qty);

TEST(Db2ArrayBinding, MultiRowInsertAggregatesCountAndNulls) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";
    auto dbh = halcyon::Database::open(*d);
    ASSERT_TRUE(dbh.ok()) << dbh.error().message;
    auto& h = dbh.value();
    h.execute("DROP TABLE halcyon_arr");  // ignore if absent
    ASSERT_TRUE(h.execute(
                     "CREATE TABLE halcyon_arr(id BIGINT NOT NULL, "
                     "name VARCHAR(32), qty BIGINT)")
                    .ok());

    std::vector<ArrRow> rows = {
        {1, "a", 10}, {2, "b", std::nullopt}, {3, "c", 30},
        {4, "d", 40}, {5, "e", std::nullopt},
    };
    auto n = h.executeBatch(
        "INSERT INTO halcyon_arr(id,name,qty) VALUES (?,?,?)",
        halcyon::batchOf(rows));
    ASSERT_TRUE(n.ok()) << n.error().message;
    EXPECT_EQ(n.value(), 5);

    auto count = h.queryAsOrThrow<BatchCount>(
        "SELECT COUNT(*) AS c FROM halcyon_arr");
    ASSERT_EQ(count.size(), 1u);
    EXPECT_EQ(count[0].c, 5);

    auto nulls = h.queryAsOrThrow<BatchCount>(
        "SELECT COUNT(*) AS c FROM halcyon_arr WHERE qty IS NULL");
    EXPECT_EQ(nulls[0].c, 2);

    h.execute("DROP TABLE halcyon_arr");
}

TEST(Db2ArrayBinding, ConstraintViolationReportsRowIndex) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";
    auto dbh = halcyon::Database::open(*d);
    ASSERT_TRUE(dbh.ok()) << dbh.error().message;
    auto& h = dbh.value();
    h.execute("DROP TABLE halcyon_arr_pk");
    ASSERT_TRUE(h.execute(
                     "CREATE TABLE halcyon_arr_pk(id BIGINT NOT NULL PRIMARY KEY, "
                     "name VARCHAR(32))")
                    .ok());

    // Row index 2 (0-based) duplicates id=1 -> constraint violation.
    auto batch = halcyon::batchOf({
        std::make_tuple(std::int64_t{1}, std::string{"a"}),
        std::make_tuple(std::int64_t{2}, std::string{"b"}),
        std::make_tuple(std::int64_t{1}, std::string{"dup"}),
    });
    auto n = h.executeBatch(
        "INSERT INTO halcyon_arr_pk(id,name) VALUES (?,?)", batch);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, halcyon::ErrorCode::Constraint);
    EXPECT_NE(n.error().message.find("row"), std::string::npos);

    h.execute("DROP TABLE halcyon_arr_pk");
}

TEST(Db2ArrayBinding, MultiChunkInsertExceedsByteBudget) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";
    auto dbh = halcyon::Database::open(*d);
    ASSERT_TRUE(dbh.ok()) << dbh.error().message;
    auto& h = dbh.value();
    h.execute("DROP TABLE halcyon_arr_big");
    ASSERT_TRUE(h.execute(
                     "CREATE TABLE halcyon_arr_big(id BIGINT NOT NULL, "
                     "payload VARCHAR(1100))")
                    .ok());

    // ~20 MB of bound data ( > 16 MiB budget ) forces at least two chunks.
    const std::int64_t kRows = 20000;
    const std::string payload(1000, 'x');
    std::vector<std::tuple<std::int64_t, std::string>> rows;
    rows.reserve(kRows);
    for (std::int64_t i = 0; i < kRows; ++i) rows.emplace_back(i, payload);

    auto n = h.executeBatch(
        "INSERT INTO halcyon_arr_big(id,payload) VALUES (?,?)",
        halcyon::batchOf(rows));
    ASSERT_TRUE(n.ok()) << n.error().message;
    EXPECT_EQ(n.value(), kRows);

    auto count = h.queryAsOrThrow<BatchCount>(
        "SELECT COUNT(*) AS c FROM halcyon_arr_big");
    EXPECT_EQ(count[0].c, kRows);

    h.execute("DROP TABLE halcyon_arr_big");
}
```

- [ ] **Step 2: Bring up live Db2 and run the new tests to verify they fail/are insufficient**

```bash
export DOCKER_DEFAULT_PLATFORM=linux/amd64
docker compose -f docker/docker-compose.yml up -d
# wait until healthy (first boot creates SAMPLE); then:
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=<pw>;"
cmake -S . -B build-it -DHALCYON_BUILD_TESTS=ON -DHALCYON_BUILD_INTEGRATION_TESTS=ON
cmake --build build-it -j
ctest --test-dir build-it --output-on-failure -L integration -R Db2ArrayBinding
```

Expected with the provisional per-row impl: `MultiRowInsert...` and `MultiChunk...` likely PASS (per-row also inserts correctly), but `ConstraintViolationReportsRowIndex` FAILS the `find("row")` assertion — the per-row loop's error message has no batch-row index. This is the red that array binding makes green.

- [ ] **Step 3: Implement true array binding**

In `src/detail/cli/db2_cli_driver.cpp`, replace the **entire** provisional `executeBatch` body (from Task 1) with the column-wise implementation below. Add `#include <cstring>` at the top if not already present.

```cpp
    Result<std::int64_t> executeBatch(
        StatementHandle stmt,
        const std::vector<std::vector<Value>>& rows) override {
        StmtState* stp = stmt_state(stmt);
        if (!stp) return unknown_stmt();
        SQLHSTMT h = stp->handle;
        const std::size_t nRows = rows.size();
        if (nRows == 0) return std::int64_t{0};
        const std::size_t nCols = rows.front().size();

        // Resolve each column's CLI type from its first non-null value. Input is
        // validated rectangular + homogeneous above the seam; an all-null column
        // defaults to VARCHAR (mirrors the scalar null path).
        std::vector<SQLSMALLINT> cType(nCols, SQL_C_CHAR);
        std::vector<SQLSMALLINT> sqlType(nCols, SQL_VARCHAR);
        std::vector<bool> isVar(nCols, true);
        std::vector<SQLLEN> fixedW(nCols, 0);
        for (std::size_t c = 0; c < nCols; ++c) {
            for (std::size_t r = 0; r < nRows; ++r) {
                const Value& v = rows[r][c];
                if (std::holds_alternative<Null>(v)) continue;
                if (std::holds_alternative<bool>(v)) {
                    cType[c] = SQL_C_BIT; sqlType[c] = SQL_BIT;
                    isVar[c] = false; fixedW[c] = 1;
                } else if (std::holds_alternative<std::int64_t>(v)) {
                    cType[c] = SQL_C_SBIGINT; sqlType[c] = SQL_BIGINT;
                    isVar[c] = false; fixedW[c] = sizeof(std::int64_t);
                } else if (std::holds_alternative<double>(v)) {
                    cType[c] = SQL_C_DOUBLE; sqlType[c] = SQL_DOUBLE;
                    isVar[c] = false; fixedW[c] = sizeof(double);
                } else if (std::holds_alternative<std::string>(v)) {
                    cType[c] = SQL_C_CHAR; sqlType[c] = SQL_VARCHAR;
                    isVar[c] = true;
                } else {
                    cType[c] = SQL_C_BINARY; sqlType[c] = SQL_BINARY;
                    isVar[c] = true;
                }
                break;  // first non-null defines the column
            }
        }

        constexpr std::size_t kByteBudget = 16u * 1024 * 1024;
        std::int64_t total = 0;
        std::size_t start = 0;
        while (start < nRows) {
            // Grow the chunk until adding the next row would exceed the budget.
            // colMax tracks each var-width column's longest element in the chunk.
            std::vector<std::size_t> colMax(nCols, 0);
            std::size_t end = start;
            while (end < nRows) {
                std::vector<std::size_t> newMax = colMax;
                for (std::size_t c = 0; c < nCols; ++c) {
                    if (!isVar[c]) continue;
                    const Value& v = rows[end][c];
                    std::size_t len = 0;
                    if (auto* s = std::get_if<std::string>(&v)) len = s->size();
                    else if (auto* b = std::get_if<std::vector<std::byte>>(&v))
                        len = b->size();
                    if (len > newMax[c]) newMax[c] = len;
                }
                const std::size_t prospectiveRows = end - start + 1;
                std::size_t bytes = 0;
                for (std::size_t c = 0; c < nCols; ++c) {
                    std::size_t w = isVar[c]
                        ? (newMax[c] == 0 ? 1 : newMax[c])
                        : static_cast<std::size_t>(fixedW[c]);
                    bytes += w * prospectiveRows;
                }
                if (end > start && bytes > kByteBudget) break;  // keep >= 1 row
                colMax = newMax;
                ++end;
            }
            const std::size_t chunkRows = end - start;

            // Column-wise buffers + indicator arrays; must outlive SQLExecute.
            std::vector<std::vector<char>> buf(nCols);
            std::vector<std::vector<SQLLEN>> ind(nCols);
            std::vector<SQLLEN> width(nCols, 0);
            for (std::size_t c = 0; c < nCols; ++c) {
                width[c] = isVar[c]
                    ? static_cast<SQLLEN>(colMax[c] == 0 ? 1 : colMax[c])
                    : fixedW[c];
                buf[c].assign(static_cast<std::size_t>(width[c]) * chunkRows, 0);
                ind[c].assign(chunkRows, 0);
                for (std::size_t i = 0; i < chunkRows; ++i) {
                    const Value& v = rows[start + i][c];
                    char* slot = buf[c].data() +
                                 static_cast<std::size_t>(width[c]) * i;
                    if (std::holds_alternative<Null>(v)) {
                        ind[c][i] = SQL_NULL_DATA;
                    } else if (auto* b = std::get_if<bool>(&v)) {
                        *slot = static_cast<char>(*b ? 1 : 0);
                    } else if (auto* p = std::get_if<std::int64_t>(&v)) {
                        std::memcpy(slot, p, sizeof(*p));
                    } else if (auto* dd = std::get_if<double>(&v)) {
                        std::memcpy(slot, dd, sizeof(*dd));
                    } else if (auto* s = std::get_if<std::string>(&v)) {
                        std::memcpy(slot, s->data(), s->size());
                        ind[c][i] = static_cast<SQLLEN>(s->size());
                    } else {
                        const auto& by = std::get<std::vector<std::byte>>(v);
                        std::memcpy(slot, by.data(), by.size());
                        ind[c][i] = static_cast<SQLLEN>(by.size());
                    }
                }
            }

            std::vector<SQLUSMALLINT> status(chunkRows, 0);
            SQLULEN processed = 0;
            SQLSetStmtAttr(h, SQL_ATTR_PARAMSET_SIZE,  // NOLINT(performance-no-int-to-ptr)
                reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(chunkRows)), 0);
            SQLSetStmtAttr(h, SQL_ATTR_PARAM_BIND_TYPE,  // NOLINT(performance-no-int-to-ptr)
                reinterpret_cast<SQLPOINTER>(
                    static_cast<SQLULEN>(SQL_PARAM_BIND_BY_COLUMN)), 0);
            SQLSetStmtAttr(h, SQL_ATTR_PARAM_STATUS_PTR, status.data(), 0);
            SQLSetStmtAttr(h, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0);

            for (std::size_t c = 0; c < nCols; ++c) {
                SQLRETURN rc = SQLBindParameter(
                    h, static_cast<SQLUSMALLINT>(c + 1), SQL_PARAM_INPUT,
                    cType[c], sqlType[c], static_cast<SQLULEN>(width[c]), 0,
                    buf[c].data(), width[c], ind[c].data());
                if (!cli_ok(rc)) {
                    Error e = make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                         "SQLBindParameter (array) failed");
                    reset_paramset(h);
                    return e;
                }
            }

            SQLRETURN rc = SQLExecute(h);
            if (!cli_ok(rc) && rc != SQL_NO_DATA) {
                std::size_t firstErr = chunkRows;
                for (std::size_t i = 0; i < chunkRows; ++i)
                    if (status[i] == SQL_PARAM_ERROR) { firstErr = i; break; }
                Error e = make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                     "SQLExecute (array) failed");
                if (firstErr < chunkRows)
                    e.message +=
                        " at batch row " + std::to_string(start + firstErr);
                reset_paramset(h);
                return e;
            }
            SQLLEN affected = 0;
            SQLRowCount(h, &affected);
            total += affected < 0 ? 0 : static_cast<std::int64_t>(affected);
            start = end;
        }
        reset_paramset(h);
        return total;
    }
```

Add this private helper next to the other helpers (e.g. after `end_tran`, ~line 411):

```cpp
    // Restores single-row paramset state so a cached statement is clean for
    // later scalar bindParams/execute reuse.
    static void reset_paramset(SQLHSTMT h) {
        SQLSetStmtAttr(h, SQL_ATTR_PARAMSET_SIZE,  // NOLINT(performance-no-int-to-ptr)
            reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(1)), 0);
        SQLSetStmtAttr(h, SQL_ATTR_PARAM_STATUS_PTR, nullptr, 0);
        SQLSetStmtAttr(h, SQL_ATTR_PARAMS_PROCESSED_PTR, nullptr, 0);
    }
```

- [ ] **Step 4: Rebuild and run the array-binding integration tests**

```bash
cmake --build build-it -j
ctest --test-dir build-it --output-on-failure -L integration -R Db2ArrayBinding
```
Expected: all three `Db2ArrayBinding.*` tests PASS, including the row-index assertion.

- [ ] **Step 5: Run the full integration suite for regressions**

Run: `ctest --test-dir build-it --output-on-failure -L integration`
Expected: all live tests PASS (existing `ErgonomicOpenBatchAndAsync` etc. unaffected).

- [ ] **Step 6: Run the unit suite (mock path unchanged)**

Run: `ctest --test-dir build --output-on-failure`
Expected: all PASS.

- [ ] **Step 7: Commit**

```bash
git add src/detail/cli/db2_cli_driver.cpp tests/integration/test_db2_roundtrip.cpp
git commit -m "feat: true Db2 CLI array binding for executeBatch with byte-budget chunking"
```

---

## Task 3: Batch-insert throughput benchmark (live)

A standalone benchmark in the perf dir that times per-row vs array vs transaction-wrapped insert, validating spec §8.

**Files:**
- Create: `tests/stress/perf/batch_insert_bench.cpp`
- Modify: `tests/stress/perf/CMakeLists.txt`

- [ ] **Step 1: Write the benchmark**

Create `tests/stress/perf/batch_insert_bench.cpp`:

```cpp
// Live batch-insert throughput benchmark (spec §8/§9). Requires HALCYON_TEST_DSN.
// Times inserting N rows three ways and prints rows/sec:
//   1) per-row execute() under autocommit
//   2) executeBatch (array binding) under autocommit
//   3) executeBatch wrapped in a transaction (one commit)
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#include "halcyon/halcyon.hpp"

namespace {
double secs_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
        .count();
}
void recreate(halcyon::Database& h) {
    h.execute("DROP TABLE halcyon_bench");
    h.executeOrThrow(
        "CREATE TABLE halcyon_bench(id BIGINT NOT NULL, payload VARCHAR(64))");
}
}  // namespace

int main() {
    const char* dsn = std::getenv("HALCYON_TEST_DSN");
    if (!dsn) {
        std::cerr << "HALCYON_TEST_DSN not set; skipping benchmark\n";
        return 0;
    }
    auto h = halcyon::Database::openOrThrow(std::string(dsn));
    const std::int64_t N = 50000;
    const std::string payload(48, 'x');

    std::vector<std::tuple<std::int64_t, std::string>> rows;
    rows.reserve(N);
    for (std::int64_t i = 0; i < N; ++i) rows.emplace_back(i, payload);
    const char* kSql = "INSERT INTO halcyon_bench(id,payload) VALUES (?,?)";

    // 1) per-row execute under autocommit
    recreate(h);
    auto t0 = std::chrono::steady_clock::now();
    for (std::int64_t i = 0; i < N; ++i)
        h.executeOrThrow(kSql, i, payload);
    double perRow = secs_since(t0);

    // 2) array binding under autocommit
    recreate(h);
    t0 = std::chrono::steady_clock::now();
    h.executeBatch(kSql, halcyon::batchOf(rows)).value();
    double array = secs_since(t0);

    // 3) array binding inside one transaction
    recreate(h);
    t0 = std::chrono::steady_clock::now();
    {
        auto tx = h.begin().value();
        tx.executeBatch(kSql, halcyon::batchOf(rows)).value();
        tx.commit().value();
    }
    double arrayTxn = secs_since(t0);

    h.execute("DROP TABLE halcyon_bench");

    auto rps = [N](double s) { return s > 0 ? static_cast<double>(N) / s : 0; };
    std::cout << "rows=" << N << "\n"
              << "per_row_autocommit : " << perRow << "s  "
              << rps(perRow) << " rows/s\n"
              << "array_autocommit   : " << array << "s  "
              << rps(array) << " rows/s\n"
              << "array_transaction  : " << arrayTxn << "s  "
              << rps(arrayTxn) << " rows/s\n";
    return 0;
}
```

API used (all verified in `include/halcyon/database.hpp`): `Database::openOrThrow`,
`Database::executeOrThrow(sql, args...)`, `Database::executeBatch`,
`Database::begin()` → `ScopedTransaction`, and `ScopedTransaction::executeBatch`
/ `commit` (the `executeBatch` overload is added in Task 1, Step 5).

- [ ] **Step 2: Wire the build target**

In `tests/stress/perf/CMakeLists.txt`, add (mirror the existing perf target's link/include lines):

```cmake
add_executable(halcyon_batch_insert_bench batch_insert_bench.cpp)
target_link_libraries(halcyon_batch_insert_bench PRIVATE halcyon::halcyon)
```

- [ ] **Step 3: Build the benchmark**

Run: `cmake --build build-it --target halcyon_batch_insert_bench -j`
Expected: links cleanly.

- [ ] **Step 4: Commit (running it happens in Task 9)**

```bash
git add tests/stress/perf/batch_insert_bench.cpp tests/stress/perf/CMakeLists.txt
git commit -m "test: live batch-insert throughput benchmark (per-row vs array vs txn)"
```

---

## Task 4: Reconcile docs to array binding

**Files:**
- Modify: `docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md` (§5 Bulk/batch, line ~188)
- Modify: `docs/guide/batch-operations.md` (line 1–3)
- Modify: `README.md`

- [ ] **Step 1: Update the parent spec §5**

In `docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md`, replace the "Bulk / batch (v1 in-scope)" block (line ~188) with text that keeps it v1 in-scope but describes array binding and the failure contract:

````markdown
### Bulk / batch (v1 in-scope)

`executeBatch` binds many rows as a Db2 CLI **column-wise parameter array** and
executes them in a single `SQLExecute` per byte-budget chunk — not per row.

```cpp
db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batchOf(rowsVector));
```

Any row failure surfaces as a single `Error` (message includes the first failing
row index); wrap the call in a transaction for all-or-nothing and re-drive the
whole batch on error. See
`docs/superpowers/specs/2026-06-26-halcyon-array-binding-design.md`.
````

- [ ] **Step 2: Update the batch guide**

In `docs/guide/batch-operations.md`, replace the opening sentence (lines 1–3):

```markdown
# Batch Operations

`Database::executeBatch` inserts multiple rows with a single prepared statement
using Db2 CLI **array binding**: rows are bound column-wise and sent to the
server in one `SQLExecute` per chunk, returning the total affected-row count.
```

Then append a section at the end of the file:

```markdown
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
```

- [ ] **Step 3: Update the README**

Search the README for per-row batch wording and align it:

Run: `grep -n "executeBatch\|per row\|once per row\|batch" README.md`

Edit any line describing `executeBatch` as per-row execution to say it uses Db2
CLI array binding (column-wise, one execute per chunk). If the README has no such
wording, note that in the commit and skip.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md \
        docs/guide/batch-operations.md README.md
git commit -m "docs: describe executeBatch as true Db2 CLI array binding"
```

---

## Task 5: Plan/spec drift cleanup (stale checkboxes)

Marks the two completed plans done. **Verify completeness before checking** — do not blindly tick boxes.

**Files:**
- Modify: `docs/superpowers/plans/2026-06-24-halcyon-otel-context-propagation.md`
- Modify: `docs/superpowers/plans/2026-06-22-halcyon-orders-sample.md`

- [ ] **Step 1: Verify the OTel propagation work landed**

```bash
git log --oneline | grep -iE 'otel|parent context|propagat'
ls include/halcyon/observability/otel_adapter.hpp tests/unit/test_otel_propagation.cpp
grep -rn 'useParentContext' include/halcyon/database.hpp
ctest --test-dir build --output-on-failure -R 'Otel|Propagat|Trac'
```
Expected: commits `7fc521b`..`b16e41f` present, files exist, tests pass. Only proceed if all confirm.

- [ ] **Step 2: Check the OTel plan boxes**

In `docs/superpowers/plans/2026-06-24-halcyon-otel-context-propagation.md`, change every `- [ ]` whose task is confirmed landed to `- [x]`.

Run: `sed -i '' 's/- \[ \]/- [x]/g' docs/superpowers/plans/2026-06-24-halcyon-otel-context-propagation.md`
Then visually confirm no task remains that was NOT actually done. (If any task is genuinely incomplete, revert that line to `- [ ]`.)

- [ ] **Step 3: Verify the orders sample work landed**

```bash
ls -R samples/orders
test -f samples/orders/src/orders_oo.cpp && test -f samples/orders/src/orders_functional.cpp \
  && test -f samples/orders/sql/schema.sql && test -f samples/orders/sql/seed.sql \
  && test -f samples/orders/load_sql.sh && echo "orders sample present"
```
Expected: all files present. (End-to-end run happens in Task 8; presence is enough to mark the authoring tasks done. Leave the "run against live Db2" sub-steps unchecked until Task 8 confirms them.)

- [ ] **Step 4: Check the orders plan boxes (authoring steps only)**

In `docs/superpowers/plans/2026-06-22-halcyon-orders-sample.md`, tick `- [x]` for every "Write …", "Make executable", and "Commit" step whose artifact exists. Leave any "Run against live Db2 / verify output / verify rerunnability" step `- [ ]` until Task 8 verifies it live, then return here and tick those.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/plans/2026-06-24-halcyon-otel-context-propagation.md \
        docs/superpowers/plans/2026-06-22-halcyon-orders-sample.md
git commit -m "docs: mark completed OTel-propagation and orders-sample plan tasks done"
```

---

## Task 6: Full live Db2 integration run

**Files:** none (verification only).

- [ ] **Step 1: Ensure Db2 is up (amd64 emulation on this Mac)**

```bash
export DOCKER_DEFAULT_PLATFORM=linux/amd64
docker compose -f docker/docker-compose.yml up -d
docker compose -f docker/docker-compose.yml ps   # wait for healthy
```
If the GSKit/driver libs are quarantined on macOS, clear quarantine per
`memory: db2-integration-arm64-docker` before running.

- [ ] **Step 2: Configure + build with integration tests**

```bash
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=<pw>;"
cmake -S . -B build-it -DHALCYON_BUILD_TESTS=ON -DHALCYON_BUILD_INTEGRATION_TESTS=ON
cmake --build build-it -j
```

- [ ] **Step 3: Run the full integration suite**

Run: `ctest --test-dir build-it --output-on-failure -L integration`
Expected: all live tests PASS (no `GTEST_SKIP`). Capture the summary into the
final report. If any test flakes on Db2 boot races, re-run once after `ps` shows
healthy (see commit `b379525` context).

---

## Task 7: TSan stress run

**Files:** none (verification only).

- [ ] **Step 1: Build the stress suite under ThreadSanitizer**

Follow `tests/stress/README.md` for the exact invocation. Per
`memory: halcyon-sanitizer-build-workflow`, use a separate build dir and reuse
the fetched gtest:

```bash
cmake -S . -B build-tsan -DHALCYON_BUILD_TESTS=ON -DHALCYON_BUILD_STRESS_TESTS=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$PWD/cmake-build-debug/_deps/googletest-src"
cmake --build build-tsan -j
```
(Use the exact target/option names from `tests/stress/README.md` and the stress
`CMakeLists.txt` if they differ.)

- [ ] **Step 2: Run the stress correctness suite under TSan**

Run: `ctest --test-dir build-tsan --output-on-failure -L stress`
Expected: PASS with no ThreadSanitizer data-race reports. Capture any TSan
output verbatim into the report.

---

## Task 8: Orders sample end-to-end

**Files:** none (verification only); may return to Task 5 Step 4 to tick live boxes.

- [ ] **Step 1: Load the orders schema + seed into live Db2**

```bash
export DOCKER_DEFAULT_PLATFORM=linux/amd64
bash samples/orders/load_sql.sh
```
Follow `samples/orders/README.md` for any prerequisites. Expected: schema +
seed load without error.

- [ ] **Step 2: Configure + build the sample against the built Halcyon**

```bash
cmake -S samples/orders -B samples/orders/build \
  -DCMAKE_PREFIX_PATH="$PWD/build-it" \
  -DDB2_CLIDRIVER_ROOT="$PWD/third_party/clidriver"
cmake --build samples/orders/build -j
```
(Use the exact flags from `samples/orders/README.md`.)

- [ ] **Step 3: Run both walkthroughs against live Db2**

```bash
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=<pw>;"
./samples/orders/build/orders_oo
./samples/orders/build/orders_functional
```
Expected: both print the documented walkthrough output and exit 0; the
functional output matches the OO output. Re-run to confirm rerunnability.

- [ ] **Step 4: Tick the live boxes in the orders plan**

Return to `docs/superpowers/plans/2026-06-22-halcyon-orders-sample.md` and change
the now-verified "Run against live Db2 / verify output / verify rerunnability"
steps to `- [x]`. Commit:

```bash
git add docs/superpowers/plans/2026-06-22-halcyon-orders-sample.md
git commit -m "docs: mark orders-sample live-run steps verified"
```

---

## Task 9: Run the benchmark + record numbers

**Files:**
- Modify: `docs/superpowers/specs/2026-06-26-halcyon-array-binding-design.md` (§9)

- [ ] **Step 1: Run the benchmark against live Db2**

```bash
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=<pw>;"
./build-it/tests/stress/perf/halcyon_batch_insert_bench   # path per your build layout
```
Expected: three rows/sec figures. Array and array-transaction should both
exceed per-row; array-transaction should be the fastest (one commit).

- [ ] **Step 2: Record results in the spec**

Append the measured figures to §9 "Empirical validation" of
`docs/superpowers/specs/2026-06-26-halcyon-array-binding-design.md` as a short
results table (date, row count, the three rows/sec numbers, one-line takeaway).
If results contradict §8 (e.g. array not faster), investigate before claiming
done — use superpowers:systematic-debugging.

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-06-26-halcyon-array-binding-design.md
git commit -m "docs: record array-binding benchmark results"
```

---

## Task 10: CI parity

**Files:**
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: Audit CI vs local verification**

Run: `cat .github/workflows/ci.yml`
List which of these the workflow runs and which it omits: unit tests, integration
tests (Dockerized Db2), stress/sanitizer (TSan/ASan), examples build, the orders
sample build, warnings-as-errors. Note each gap.

- [ ] **Step 2: Close the gaps**

Edit `.github/workflows/ci.yml` to add the missing steps, mirroring the local
commands from Tasks 6–8 and AGENTS.md. Keep heavy amd64-emulated Db2 integration
in a job that matches existing CI capability (it may already gate on a service
container — preserve that pattern; see commit `b379525` for the Db2 race
workaround). Do not invent infrastructure the runner can't provide; if live Db2
isn't feasible in CI, ensure at least unit + stress + examples + warnings-as-
errors run, and document the integration-tests-are-local caveat in a comment.

- [ ] **Step 3: Validate the workflow locally if possible**

If `act` or a YAML linter is available, run it; otherwise verify by inspection
that step syntax matches existing jobs.

Run: `git diff .github/workflows/ci.yml`
Expected: additive, consistent with existing job style.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: align workflow with local verification (unit/stress/examples)"
```

---

## Final verification

- [ ] **Run the full unit suite:** `ctest --test-dir build --output-on-failure` — all PASS
- [ ] **Run the full integration suite:** `ctest --test-dir build-it --output-on-failure -L integration` — all PASS, none skipped
- [ ] **TSan stress clean:** no data-race reports
- [ ] **Orders sample E2E:** both binaries run green and rerunnable
- [ ] **Benchmark recorded:** array ≥ per-row, transaction fastest, numbers in spec §9
- [ ] **No stale `- [ ]`** in the OTel/orders plans for work that is actually done
- [ ] Use superpowers:requesting-code-review before merging the branch
