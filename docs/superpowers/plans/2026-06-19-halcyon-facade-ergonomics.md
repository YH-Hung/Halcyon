# Halcyon Facade & Ergonomics Implementation Plan (Plan 4)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the user-facing **facade** on top of the Plan 3 pool: a thread-safe
`Database` entry point (`open`/`openOrThrow`) with OO `query`/`execute`/`queryAs`
(dual `Result<T>` + throwing overloads), a RAII `Transaction` plus the functional
`transaction(...)` helper, safe auto-retry wiring (`ExecPolicy` + `is_read_only`),
bulk `executeBatch`/`batchOf`, async `queryAsync`/`executeAsync`, and functional
free functions — matching the spec's public API surface (§5).

**Architecture:** Everything new lives in the **facade** layer
(`include/halcyon/{database,transaction}.hpp`), strictly above the pool
(`ConnectionPool`/`PooledConnection`), core (`Connection`), and the async
`Executor`. The facade may depend on all lower layers (it is the only layer that
bundles a `PooledConnection` lifetime with a `ResultSet` cursor, via the
facade-owned `QueryResult`). Transactions need a primitive the seam does not have
yet, so this plan **extends `detail::cli::ICliDriver`** with `setAutoCommit` /
`commit` / `rollback` (the only seam change), implemented in the real
`Db2CliDriver` via `SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT)` + `SQLEndTran`, and in
`MockCliDriver` as scriptable call counters. The invariant from `AGENTS.md` holds:
only `src/detail/cli/` sees `sqlcli1.h`; the whole facade is unit-tested against
`MockCliDriver` with no live Db2.

**Tech Stack:** C++17, CMake ≥ 3.20, GoogleTest (FetchContent, from Plan 1),
`<future>`/`<thread>` for async (the Plan 3 `Executor`). No new third-party
dependencies. The vendored IBM Db2 CLI driver is only touched by the (gated)
integration test and the new `Db2CliDriver` transaction methods.

**Builds on (Plans 1–3, merged):**
`include/halcyon/{version,error,result,types,parameters,connection,retry,pool,
async}.hpp`, `include/halcyon/detail/cli/{driver,sqlstate,db2_cli_driver}.hpp`,
`src/detail/cli/db2_cli_driver.cpp`, `tests/unit/mock_cli_driver.hpp`, the CMake
target `halcyon::halcyon`, and the GoogleTest harness.

---

## Design contracts (shared across tasks — keep consistent)

These names/signatures are referenced by multiple tasks. Treat them as the source
of truth; if a later task needs a change, update this section in the same commit.

**Seam transaction primitives (Task 1, `driver.hpp`):**

```cpp
// Added to detail::cli::ICliDriver (pure virtual):
virtual Result<void> setAutoCommit(ConnectionHandle conn, bool enabled) = 0;
virtual Result<void> commit(ConnectionHandle conn) = 0;
virtual Result<void> rollback(ConnectionHandle conn) = 0;
```

**Transaction (Task 2, `transaction.hpp`):**

```cpp
class Transaction {  // move-only RAII; autocommit OFF for its lifetime
    // begun via Connection::begin(); commit on success, auto-rollback otherwise.
    template <class... A> Result<std::int64_t> execute(const std::string&, const A&...);
    Result<std::int64_t> execute(const std::string&, const params&);
    template <class... A> Result<ResultSet> query(const std::string&, const A&...);
    Result<ResultSet> query(const std::string&, const params&);
    template <class T, class... A> Result<std::vector<T>> queryAs(const std::string&, const A&...);
    Result<void> commit();    // commits, restores autocommit, disarms rollback
    Result<void> rollback();  // explicit rollback
    bool active() const noexcept;
};
// Connection gains:
Result<Transaction> begin();   // setAutoCommit(false) then construct
```

**Facade (Task 3, `database.hpp`):**

```cpp
class QueryResult {            // bundles the lease + cursor so the row range is
    // valid while iterating; move-only. begin()/end() forward to the ResultSet.
    auto begin(); auto end();
    ResultSet& rows() noexcept;
};

class Database {               // thread-safe; copyable handle (shared pool)
    // Ergonomic (spec §5): the facade creates and owns the real Db2 CLI driver.
    static Result<Database> open(const std::string& dsn, PoolConfig = {});
    static Database openOrThrow(const std::string& dsn, PoolConfig = {});
    // Injectable overloads (first arg detail::cli::ICliDriver&) let unit tests
    // drive the facade against MockCliDriver; the caller owns that driver.
    static Result<Database> open(detail::cli::ICliDriver&, const std::string& dsn, PoolConfig = {});
    static Database openOrThrow(detail::cli::ICliDriver&, const std::string& dsn, PoolConfig = {});

    template <class... A> Result<QueryResult> query(const std::string&, const A&...);
    Result<QueryResult> query(const std::string&, const params&);
    template <class... A> Result<std::int64_t> execute(const std::string&, const A&...);
    Result<std::int64_t> execute(const std::string&, const params&);
    template <class T, class... A> Result<std::vector<T>> queryAs(const std::string&, const A&...);
    template <class T> Result<std::vector<T>> queryAs(const std::string&, const params&);

    // Throwing overloads (suffix "OrThrow") unwrap via Result::value().
    template <class... A> QueryResult queryOrThrow(const std::string&, const A&...);
    template <class... A> std::int64_t executeOrThrow(const std::string&, const A&...);
    template <class T, class... A> std::vector<T> queryAsOrThrow(const std::string&, const A&...);

    // Transactions (Task 5 adds the functional form).
    Result<Transaction> begin();
    template <class Fn> auto transaction(Fn&& fn) -> Result<...>;

    // Batch (Task 6) and async (Task 7) added later.
    ConnectionPool& pool() noexcept;
};
```

**Per-call retry policy (Task 4):** `Database::query`/`execute` accept an optional
trailing `ExecPolicy`; default is derived — read-only SQL (`detail::is_read_only`)
gets `ExecPolicy::idempotent(pool default attempts)`, writes get `ExecPolicy::once()`.
A connection-class error (`ErrorCode::Connection`) marks the lease broken before
retrying on a fresh connection.

**Functional free functions (Task 5, `database.hpp`):**

```cpp
namespace halcyon {
template <class... A> Result<QueryResult> query(Database&, const std::string&, const A&...);
template <class... A> Result<std::int64_t> execute(Database&, const std::string&, const A&...);
template <class T, class... A> Result<std::vector<T>> query_as(Database&, const std::string&, const A&...);
template <class Fn> auto transaction(Database&, Fn&& fn);
}
```

**Batch (Task 6, `database.hpp`):**

```cpp
template <class T> std::vector<std::vector<detail::cli::Value>> batchOf(const std::vector<T>& rows);  // for HALCYON_REFLECT'd T
// plus an initializer-style helper for tuples:
template <class... Cols> Batch batchOf(std::initializer_list<std::tuple<Cols...>> rows);
Result<std::int64_t> Database::executeBatch(const std::string& sql, const Batch& batch);
```

**Async (Task 7, `database.hpp`):**

```cpp
// Typed materializing async query (spec §5 name): future carries owned rows.
template <class T, class... A> std::future<Result<std::vector<T>>> Database::queryAsync(...);
template <class... A> std::future<Result<std::int64_t>> Database::executeAsync(...);
// Untyped/streaming queryAsync over a live cursor across threads is a documented
// non-goal (the future would tie a pool lease to another thread).
```

---

## File Structure (created/modified by this plan)

- `include/halcyon/detail/cli/driver.hpp` — **modify**: add 3 transaction methods.
- `src/detail/cli/db2_cli_driver.cpp` — **modify**: implement them over CLI.
- `tests/unit/mock_cli_driver.hpp` — **modify**: scriptable transaction calls.
- `include/halcyon/transaction.hpp` — **create**: `Transaction` (RAII).
- `include/halcyon/connection.hpp` — **modify**: add `Connection::begin()`.
- `include/halcyon/database.hpp` — **create**: `QueryResult`, `Database`, batch,
  functional free functions, async.
- `include/halcyon/halcyon.hpp` — **modify**: include the new headers.
- `tests/unit/test_transaction.cpp` — **create**.
- `tests/unit/test_database.cpp` — **create** (query/execute/queryAs + throwing).
- `tests/unit/test_database_retry.cpp` — **create**.
- `tests/unit/test_database_batch.cpp` — **create**.
- `tests/unit/test_database_async.cpp` — **create**.
- `tests/CMakeLists.txt` — **modify**: register the new unit tests.
- `examples/CMakeLists.txt` — **create**; `examples/oo_usage.cpp`,
  `examples/functional_usage.cpp` — **create**.
- `CMakeLists.txt` — **modify**: add `examples/` subdir when `HALCYON_BUILD_EXAMPLES`.
- `tests/integration/test_db2_roundtrip.cpp` — **modify**: gated facade +
  transaction tests.

---

## Task 1: Seam transaction primitives

Add `setAutoCommit` / `commit` / `rollback` to `ICliDriver` and implement them in
both the mock (scriptable counters) and the real `Db2CliDriver`
(`SQLSetConnectAttr` + `SQLEndTran`). Pure plumbing; pins behavior with a
mock-driven unit test (the real path is exercised by the gated integration test in
Task 8).

**Files:**
- Modify: `include/halcyon/detail/cli/driver.hpp`
- Modify: `src/detail/cli/db2_cli_driver.cpp`
- Modify: `tests/unit/mock_cli_driver.hpp`
- Modify: `tests/CMakeLists.txt` (add `unit/test_transaction.cpp`)
- Test: `tests/unit/test_transaction.cpp` (seam portion)

- [ ] **Step 1: Write the failing test (seam calls through the mock)**

Create `tests/unit/test_transaction.cpp`:

```cpp
#include <gtest/gtest.h>

#include "halcyon/connection.hpp"
#include "halcyon/transaction.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Connection;
using halcyon::Transaction;
using halcyon::testing::MockCliDriver;

namespace {
Connection open(MockCliDriver& d) {
    return Connection::open(d, {"x"}).value();
}
}  // namespace

TEST(SeamTransaction, MockRecordsAutoCommitCommitRollback) {
    MockCliDriver driver;
    auto conn = open(driver);
    auto h = conn.handle();
    ASSERT_TRUE(driver.setAutoCommit(h, false).ok());
    ASSERT_TRUE(driver.commit(h).ok());
    ASSERT_TRUE(driver.rollback(h).ok());
    EXPECT_EQ(driver.autoCommitCalls.size(), 1u);
    EXPECT_FALSE(driver.autoCommitCalls.front());
    EXPECT_EQ(driver.commitCalls, 1);
    EXPECT_EQ(driver.rollbackCalls, 1);
}
```

- [ ] **Step 2: Run test to verify it fails**

Add `unit/test_transaction.cpp` to `tests/CMakeLists.txt` (Task 8 shows the final
list), reconfigure, build. Expected: compile FAIL — `halcyon/transaction.hpp` not
found and `MockCliDriver` lacks the new methods. (The `transaction.hpp` include is
added now so Task 2 can append to the same test file; if isolating Task 1, comment
the include and the `Transaction` using-line until Task 2.)

- [ ] **Step 3: Extend the seam interface**

In `include/halcyon/detail/cli/driver.hpp`, add three pure-virtual methods to
`ICliDriver` (after `finalize`, before the closing brace):

```cpp
    // --- Transaction control (Plan 4) ---

    // Enables/disables autocommit on the connection. A transaction begins by
    // disabling it and ends (commit/rollback) by re-enabling it.
    virtual Result<void> setAutoCommit(ConnectionHandle conn, bool enabled) = 0;

    // Commits / rolls back the current unit of work on the connection.
    virtual Result<void> commit(ConnectionHandle conn) = 0;
    virtual Result<void> rollback(ConnectionHandle conn) = 0;
```

- [ ] **Step 4: Implement in `MockCliDriver`**

In `tests/unit/mock_cli_driver.hpp`, add scripting state (near the other counters)
and the three overrides (after `finalize`):

```cpp
    // --- transaction scripting (Plan 4) ---
    std::deque<Error> txnErrors;     // next setAutoCommit/commit/rollback fails
    std::vector<bool> autoCommitCalls;
    int commitCalls = 0;
    int rollbackCalls = 0;

    Result<void> setAutoCommit(ConnectionHandle handle, bool enabled) override {
        (void)handle;
        autoCommitCalls.push_back(enabled);
        return next_txn_result();
    }
    Result<void> commit(ConnectionHandle handle) override {
        (void)handle;
        ++commitCalls;
        return next_txn_result();
    }
    Result<void> rollback(ConnectionHandle handle) override {
        (void)handle;
        ++rollbackCalls;
        return next_txn_result();
    }
```

and a private helper (next to `rangeError()`):

```cpp
    Result<void> next_txn_result() {
        if (!txnErrors.empty()) {
            Error e = txnErrors.front();
            txnErrors.pop_front();
            return Result<void>(e);
        }
        return Result<void>();
    }
```

- [ ] **Step 5: Implement in `Db2CliDriver`**

In `src/detail/cli/db2_cli_driver.cpp`, add the three overrides after
`finalize(...)` (and before the closing `private:` block). Use the connection's
`SQLHDBC`:

```cpp
    Result<void> setAutoCommit(ConnectionHandle conn, bool enabled) override {
        auto it = conns_.find(conn);
        if (it == conns_.end()) return unknown_conn();
        SQLPOINTER v = reinterpret_cast<SQLPOINTER>(static_cast<SQLLEN>(
            enabled ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF));
        SQLRETURN rc = SQLSetConnectAttr(it->second, SQL_ATTR_AUTOCOMMIT, v,
                                         SQL_IS_INTEGER);
        if (!cli_ok(rc))
            return make_error(SQL_HANDLE_DBC, it->second, ErrorCode::Unknown,
                              "SQLSetConnectAttr(AUTOCOMMIT) failed");
        return Result<void>();
    }
    Result<void> commit(ConnectionHandle conn) override {
        return end_tran(conn, SQL_COMMIT, "SQLEndTran(COMMIT) failed");
    }
    Result<void> rollback(ConnectionHandle conn) override {
        return end_tran(conn, SQL_ROLLBACK, "SQLEndTran(ROLLBACK) failed");
    }
```

Add these private helpers in the `private:` section (next to `get_string`):

```cpp
    static Error make_conn_error(const char* msg) {
        Error e;
        e.code = ErrorCode::Connection;
        e.message = msg;
        return e;
    }
    Result<void> unknown_conn() {
        return make_conn_error("unknown connection handle");
    }
    Result<void> end_tran(ConnectionHandle conn, SQLSMALLINT how,
                          const char* context) {
        auto it = conns_.find(conn);
        if (it == conns_.end()) return unknown_conn();
        SQLRETURN rc = SQLEndTran(SQL_HANDLE_DBC, it->second, how);
        if (!cli_ok(rc))
            return make_error(SQL_HANDLE_DBC, it->second, ErrorCode::Unknown,
                              context);
        return Result<void>();
    }
```

> Note: `SQL_ATTR_AUTOCOMMIT`, `SQL_AUTOCOMMIT_ON/OFF`, `SQLSetConnectAttr`,
> `SQLEndTran`, `SQL_COMMIT`, `SQL_ROLLBACK`, `SQL_IS_INTEGER` all come from the
> already-included `sqlcli1.h`/`sqlext.h`.

- [ ] **Step 6: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R SeamTransaction --output-on-failure
```

Expected: `SeamTransaction.*` PASSES; all prior unit tests still build (the new
pure-virtuals are implemented by both drivers).

- [ ] **Step 7: Commit**

```bash
git add include/halcyon/detail/cli/driver.hpp src/detail/cli/db2_cli_driver.cpp \
        tests/unit/mock_cli_driver.hpp tests/CMakeLists.txt \
        tests/unit/test_transaction.cpp
git commit -m "feat: add transaction control (autocommit/commit/rollback) to CLI seam"
```

---

## Task 2: RAII `Transaction` + `Connection::begin`

Adds `transaction.hpp` with a move-only `Transaction` that turns off autocommit on
construction, exposes `execute`/`query`/`queryAs`, commits explicitly, and
auto-rolls-back on scope exit unless committed. `Connection::begin()` creates one.

**Files:**
- Create: `include/halcyon/transaction.hpp`
- Modify: `include/halcyon/connection.hpp` (add `begin()` + include)
- Modify: `tests/unit/test_transaction.cpp` (append)

- [ ] **Step 1: Append the failing tests**

Append to `tests/unit/test_transaction.cpp`:

```cpp
#include <cstdint>

namespace {
MockCliDriver::ScriptedRows oneInt(std::int64_t v) {
    return MockCliDriver::ScriptedRows{
        {"c"}, {{halcyon::detail::cli::Value{v}}}};
}
}  // namespace

TEST(Transaction, BeginDisablesAutoCommitCommitRestores) {
    MockCliDriver driver;
    auto conn = open(driver);
    {
        auto tx = conn.begin();
        ASSERT_TRUE(tx.ok());
        ASSERT_TRUE(tx.value().active());
        auto n = tx.value().execute("INSERT INTO t VALUES (?)", 1);
        ASSERT_TRUE(n.ok());
        ASSERT_TRUE(tx.value().commit().ok());
        EXPECT_FALSE(tx.value().active());
    }
    ASSERT_EQ(driver.autoCommitCalls.size(), 2u);
    EXPECT_FALSE(driver.autoCommitCalls[0]);  // begin → off
    EXPECT_TRUE(driver.autoCommitCalls[1]);   // commit → on
    EXPECT_EQ(driver.commitCalls, 1);
    EXPECT_EQ(driver.rollbackCalls, 0);
}

TEST(Transaction, AutoRollsBackWhenNotCommitted) {
    MockCliDriver driver;
    auto conn = open(driver);
    {
        auto tx = conn.begin().value();
        ASSERT_TRUE(tx.execute("UPDATE t SET a=?", 2).ok());
        // no commit() → dtor rolls back
    }
    EXPECT_EQ(driver.commitCalls, 0);
    EXPECT_EQ(driver.rollbackCalls, 1);
    ASSERT_EQ(driver.autoCommitCalls.size(), 2u);
    EXPECT_TRUE(driver.autoCommitCalls[1]);  // rollback restores autocommit
}

TEST(Transaction, ExplicitRollback) {
    MockCliDriver driver;
    auto conn = open(driver);
    auto tx = conn.begin().value();
    ASSERT_TRUE(tx.rollback().ok());
    EXPECT_FALSE(tx.active());
    EXPECT_EQ(driver.rollbackCalls, 1);
}

TEST(Transaction, QueryInsideTransaction) {
    MockCliDriver driver;
    driver.resultSets.push_back(oneInt(7));
    auto conn = open(driver);
    auto tx = conn.begin().value();
    auto rs = tx.query("SELECT c FROM t");
    ASSERT_TRUE(rs.ok());
    std::int64_t sum = 0;
    for (auto& row : rs.value()) sum += std::get<0>(row.as<std::int64_t>());
    EXPECT_EQ(sum, 7);
    ASSERT_TRUE(tx.commit().ok());
}

TEST(Transaction, MoveDoesNotDoubleRollback) {
    MockCliDriver driver;
    auto conn = open(driver);
    {
        auto a = conn.begin().value();
        auto b = std::move(a);  // b now owns the unit of work
        ASSERT_TRUE(b.execute("DELETE FROM t").ok());
    }
    EXPECT_EQ(driver.rollbackCalls, 1);  // exactly once, from b
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build -j
```

Expected: compile FAIL — no `Connection::begin`, no `Transaction`.

- [ ] **Step 3: Write `transaction.hpp`**

Create `include/halcyon/transaction.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "halcyon/connection.hpp"
#include "halcyon/error.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/result.hpp"

namespace halcyon {

// RAII unit of work over a single Connection. Autocommit is OFF for its
// lifetime; commit()/rollback() end it and restore autocommit. If neither is
// called before destruction, the transaction is rolled back. Move-only; never
// auto-retried (the spec requires the whole transaction to be re-driven).
class Transaction {
public:
    Transaction(Transaction&& o) noexcept
        : conn_(o.conn_), active_(o.active_) {
        o.conn_ = nullptr;
        o.active_ = false;
    }
    Transaction& operator=(Transaction&& o) noexcept {
        if (this != &o) {
            finish_rollback();
            conn_ = o.conn_;
            active_ = o.active_;
            o.conn_ = nullptr;
            o.active_ = false;
        }
        return *this;
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction() { finish_rollback(); }

    bool active() const noexcept { return active_; }

    template <class... Args>
    Result<std::int64_t> execute(const std::string& sql, const Args&... args) {
        return conn_->execute(sql, args...);
    }
    Result<std::int64_t> execute(const std::string& sql, const params& named) {
        return conn_->execute(sql, named);
    }
    template <class... Args>
    Result<ResultSet> query(const std::string& sql, const Args&... args) {
        return conn_->query(sql, args...);
    }
    Result<ResultSet> query(const std::string& sql, const params& named) {
        return conn_->query(sql, named);
    }
    template <class T, class... Args>
    Result<std::vector<T>> queryAs(const std::string& sql, const Args&... args) {
        return conn_->template queryAs<T>(sql, args...);
    }
    template <class T>
    Result<std::vector<T>> queryAs(const std::string& sql, const params& named) {
        return conn_->template queryAs<T>(sql, named);
    }

    Result<void> commit() {
        if (!active_) return Result<void>();
        auto c = conn_->driver().commit(conn_->handle());
        active_ = false;
        auto a = conn_->driver().setAutoCommit(conn_->handle(), true);
        if (!c.ok()) return c;
        return a;
    }
    Result<void> rollback() {
        if (!active_) return Result<void>();
        auto r = conn_->driver().rollback(conn_->handle());
        active_ = false;
        auto a = conn_->driver().setAutoCommit(conn_->handle(), true);
        if (!r.ok()) return r;
        return a;
    }

private:
    friend class Connection;
    explicit Transaction(Connection& conn) : conn_(&conn), active_(true) {}

    void finish_rollback() noexcept {
        if (conn_ && active_) {
            conn_->driver().rollback(conn_->handle());
            conn_->driver().setAutoCommit(conn_->handle(), true);
            active_ = false;
        }
    }

    Connection* conn_;
    bool active_;
};

}  // namespace halcyon
```

- [ ] **Step 4: Add `Connection::begin()` and a driver accessor**

In `include/halcyon/connection.hpp`, the `Transaction` type is defined in
`transaction.hpp`, which includes `connection.hpp` — so `begin()` must be declared
in `Connection` but defined out of line in `transaction.hpp` to avoid a cycle.

First, expose the driver to `Transaction`. Add a public accessor to `Connection`
(after `handle()`):

```cpp
    detail::cli::ICliDriver& driver() const noexcept { return *driver_; }
```

Then declare `begin()` inside `Connection` (after the `queryAs` overloads, before
`private:`):

```cpp
    // Begins a transaction (autocommit OFF). Defined in transaction.hpp.
    Result<Transaction> begin();
```

and forward-declare `Transaction` near the top of `connection.hpp` (after
`class ResultSet;  // fwd`):

```cpp
class Transaction;  // fwd (defined in transaction.hpp)
```

Finally, define `begin()` out of line at the bottom of `transaction.hpp` (after
the `Transaction` class, inside `namespace halcyon`):

```cpp
inline Result<Transaction> Connection::begin() {
    auto a = driver_->setAutoCommit(handle_, false);
    if (!a.ok()) return a.error();
    return Transaction(*this);
}
```

> Note: callers that use `Connection::begin()` must include `transaction.hpp`
> (the umbrella header does, per Task 8). `connection.hpp` only forward-declares
> `Transaction` and declares `begin()`, so the core header stays free of the
> facade type's definition.

- [ ] **Step 5: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R "Transaction|SeamTransaction" --output-on-failure
```

Expected: all `Transaction.*` and `SeamTransaction.*` PASS.

- [ ] **Step 6: Commit**

```bash
git add include/halcyon/transaction.hpp include/halcyon/connection.hpp \
        tests/unit/test_transaction.cpp
git commit -m "feat: add RAII Transaction with auto-rollback and Connection::begin"
```

---

## Task 3: `Database` facade — `open`, `query`/`execute`/`queryAs`, `QueryResult`

Introduces `database.hpp`. `Database` is a thread-safe handle that owns a shared
`ConnectionPool`; each call acquires a `PooledConnection`, runs, and releases.
`query(...)` returns a `QueryResult` that **owns** the lease for the cursor's
lifetime (the facade is the only layer allowed to couple pool + core). Throwing
overloads unwrap via `Result::value()`.

**Files:**
- Create: `include/halcyon/database.hpp` (core portion)
- Modify: `tests/CMakeLists.txt` (add `unit/test_database.cpp`)
- Test: `tests/unit/test_database.cpp`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_database.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "halcyon/database.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;

namespace {
MockCliDriver::ScriptedRows idName(std::int64_t id, std::string name) {
    return MockCliDriver::ScriptedRows{
        {"id", "name"},
        {{halcyon::detail::cli::Value{id},
          halcyon::detail::cli::Value{std::move(name)}}}};
}
PoolConfig noThread() {
    PoolConfig c;
    c.startMaintenanceThread = false;
    return c;
}
}  // namespace

struct Person {
    std::int64_t id;
    std::string name;
};
HALCYON_REFLECT(Person, id, name);

TEST(Database, OpenWarmsPoolToMin) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 2;
    auto db = Database::open(driver, "DATABASE=X;", cfg);
    ASSERT_TRUE(db.ok()) << db.error().message;
    EXPECT_EQ(driver.connectCalls, 2);
}

TEST(Database, ExecuteReturnsRowCount) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(3);
    auto db = Database::open(driver, "X", noThread()).value();
    auto n = db.execute("UPDATE t SET a=? WHERE id=?", 1, 5);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 3);
}

TEST(Database, QueryIteratesRows) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"},
        {{halcyon::detail::cli::Value{std::int64_t{1}}},
         {halcyon::detail::cli::Value{std::int64_t{2}}},
         {halcyon::detail::cli::Value{std::int64_t{4}}}}});
    auto db = Database::open(driver, "X", noThread()).value();
    auto qr = db.query("SELECT id FROM t WHERE id > ?", 0);
    ASSERT_TRUE(qr.ok());
    std::int64_t sum = 0;
    for (auto& row : qr.value()) sum += std::get<0>(row.as<std::int64_t>());
    EXPECT_EQ(sum, 7);
}

TEST(Database, QueryReleasesLeaseWhenResultDropped) {
    MockCliDriver driver;
    driver.resultSets.push_back(idName(1, "a"));
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    auto db = Database::open(driver, "X", cfg).value();
    { auto qr = db.query("SELECT id, name FROM t"); ASSERT_TRUE(qr.ok()); }
    // lease returned → a second single-slot query still works
    driver.resultSets.push_back(idName(2, "b"));
    auto qr2 = db.query("SELECT id, name FROM t");
    ASSERT_TRUE(qr2.ok());
}

TEST(Database, QueryAsMapsStructs) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id", "name"},
        {{halcyon::detail::cli::Value{std::int64_t{1}},
          halcyon::detail::cli::Value{std::string{"ann"}}},
         {halcyon::detail::cli::Value{std::int64_t{2}},
          halcyon::detail::cli::Value{std::string{"bob"}}}}});
    auto db = Database::open(driver, "X", noThread()).value();
    auto people = db.queryAs<Person>("SELECT id, name FROM people");
    ASSERT_TRUE(people.ok());
    ASSERT_EQ(people.value().size(), 2u);
    EXPECT_EQ(people.value()[1].name, "bob");
}

TEST(Database, NamedParamsThroughFacade) {
    MockCliDriver driver;
    driver.resultSets.push_back(idName(7, "z"));
    auto db = Database::open(driver, "X", noThread()).value();
    auto qr = db.query("SELECT id, name FROM t WHERE id = :id",
                       halcyon::params{{"id", 7}});
    ASSERT_TRUE(qr.ok());
    int rows = 0;
    for (auto& row : qr.value()) { (void)row; ++rows; }
    EXPECT_EQ(rows, 1);
}

TEST(Database, ThrowingOverloadUnwraps) {
    MockCliDriver driver;
    driver.executeErrors.push_back([] {
        halcyon::Error e;
        e.code = halcyon::ErrorCode::Syntax;
        e.message = "boom";
        return e;
    }());
    auto db = Database::open(driver, "X", noThread()).value();
    EXPECT_THROW(db.executeOrThrow("BAD SQL"), halcyon::Exception);
}
```

- [ ] **Step 2: Run test to verify it fails**

Add `unit/test_database.cpp` to `tests/CMakeLists.txt`, build. Expected: compile
FAIL — `halcyon/database.hpp` not found.

- [ ] **Step 3: Write `database.hpp` (core portion)**

Create `include/halcyon/database.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "halcyon/connection.hpp"
#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/error.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/pool.hpp"
#include "halcyon/result.hpp"
#include "halcyon/transaction.hpp"
#include "halcyon/types.hpp"

namespace halcyon {

// Owns a leased connection together with the cursor opened on it, so the row
// range stays valid for the QueryResult's lifetime. Move-only. The facade is the
// only layer permitted to couple a PooledConnection (pool) with a ResultSet
// (core); declaration order matters — lease_ is destroyed AFTER rs_.
class QueryResult {
public:
    QueryResult(QueryResult&&) noexcept = default;
    QueryResult& operator=(QueryResult&&) noexcept = default;
    QueryResult(const QueryResult&) = delete;
    QueryResult& operator=(const QueryResult&) = delete;

    ResultSet& rows() noexcept { return rs_; }
    ResultSet::iterator begin() { return rs_.begin(); }
    ResultSet::iterator end() { return rs_.end(); }
    std::size_t column_count() const noexcept { return rs_.column_count(); }

private:
    friend class Database;
    QueryResult(PooledConnection lease, ResultSet rs)
        : lease_(std::move(lease)), rs_(std::move(rs)) {}

    PooledConnection lease_;  // declared first → destroyed last (after rs_)
    ResultSet rs_;
};

// High-level thread-safe entry point. Copyable handle: copies share one pool
// (shared_ptr), so a Database can be passed by value across threads. Each call
// acquires a pooled connection, runs, and releases it on return.
class Database {
public:
    static Result<Database> open(detail::cli::ICliDriver& driver,
                                 const std::string& dsn, PoolConfig config = {}) {
        auto pool = ConnectionPool::create(driver, {dsn}, std::move(config));
        if (!pool.ok()) return pool.error();
        return Database(std::shared_ptr<ConnectionPool>(std::move(pool.value())));
    }

    static Database openOrThrow(detail::cli::ICliDriver& driver,
                                const std::string& dsn, PoolConfig config = {}) {
        return open(driver, dsn, std::move(config)).value();
    }

    ConnectionPool& pool() noexcept { return *pool_; }

    // --- execute ---
    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::int64_t> execute(const std::string& sql, const Args&... args) {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        return lease.value()->execute(sql, args...);
    }
    Result<std::int64_t> execute(const std::string& sql, const params& named) {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        return lease.value()->execute(sql, named);
    }

    // --- query (returns a lease-owning QueryResult) ---
    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<QueryResult> query(const std::string& sql, const Args&... args) {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        auto rs = lease.value()->query(sql, args...);
        if (!rs.ok()) return rs.error();
        return QueryResult(std::move(lease.value()), std::move(rs.value()));
    }
    Result<QueryResult> query(const std::string& sql, const params& named) {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        auto rs = lease.value()->query(sql, named);
        if (!rs.ok()) return rs.error();
        return QueryResult(std::move(lease.value()), std::move(rs.value()));
    }

    // --- queryAs (materialized; no lease lifetime concern) ---
    template <class T, class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::vector<T>> queryAs(const std::string& sql, const Args&... args) {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        return lease.value()->template queryAs<T>(sql, args...);
    }
    template <class T>
    Result<std::vector<T>> queryAs(const std::string& sql, const params& named) {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        return lease.value()->template queryAs<T>(sql, named);
    }

    // --- transactions ---
    // Acquires a lease, begins a transaction, and returns it bundled with the
    // lease so both live together. (Functional transaction(fn) added in Task 5.)
    Result<Transaction> begin() {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        // The lease must outlive the Transaction; bundle below in Task 5's
        // ScopedTransaction. For the RAII handle, hold the lease inside the txn
        // via begin-on-connection and keep the lease alive in a member.
        return begin_on(std::move(lease.value()));
    }

    // --- throwing overloads ---
    template <class... Args>
    QueryResult queryOrThrow(const std::string& sql, const Args&... args) {
        return query(sql, args...).value();
    }
    QueryResult queryOrThrow(const std::string& sql, const params& named) {
        return query(sql, named).value();
    }
    template <class... Args>
    std::int64_t executeOrThrow(const std::string& sql, const Args&... args) {
        return execute(sql, args...).value();
    }
    std::int64_t executeOrThrow(const std::string& sql, const params& named) {
        return execute(sql, named).value();
    }
    template <class T, class... Args>
    std::vector<T> queryAsOrThrow(const std::string& sql, const Args&... args) {
        return queryAs<T>(sql, args...).value();
    }

private:
    explicit Database(std::shared_ptr<ConnectionPool> pool)
        : pool_(std::move(pool)) {}

    Result<Transaction> begin_on(PooledConnection lease);  // Task 5 (ScopedTransaction)

    std::shared_ptr<ConnectionPool> pool_;
};

}  // namespace halcyon
```

> Note: `Database::begin()`/`begin_on()` are declared here but only fully realized
> in Task 5 (they need the lease-owning `ScopedTransaction`). To keep Task 3
> compiling and tested without transactions, provide a temporary definition at the
> end of the header (replaced in Task 5):
>
> ```cpp
> inline Result<Transaction> Database::begin_on(PooledConnection lease) {
>     // Temporary (Task 3): begins on the leased connection but the lease is
>     // released at function exit. Task 5 replaces this with ScopedTransaction
>     // that keeps the lease alive for the transaction's lifetime.
>     (void)lease;
>     Error e;
>     e.code = ErrorCode::Unknown;
>     e.message = "Database::begin not yet implemented (Task 5)";
>     return e;
> }
> ```
>
> Do not test `Database::begin()` in Task 3 — it is covered in Task 5.

- [ ] **Step 4: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R "Database\." --output-on-failure
```

Expected: all `Database.*` tests from this task PASS.

- [ ] **Step 5: Commit**

```bash
git add include/halcyon/database.hpp tests/CMakeLists.txt tests/unit/test_database.cpp
git commit -m "feat: add Database facade with lease-owning QueryResult and dual error model"
```

---

## Task 4: Safe auto-retry wiring

Wraps `Database::query`/`execute` so retriable errors on safe operations replay on
a fresh connection. Read-only SQL (`detail::is_read_only`) is auto-retried by
default; writes are not (the caller may pass `ExecPolicy::idempotent(...)` to opt
in). A connection-class error marks the lease broken so the pool discards it.

**Files:**
- Modify: `include/halcyon/database.hpp` (retry-wrap execute/query/queryAs)
- Modify: `tests/CMakeLists.txt` (add `unit/test_database_retry.cpp`)
- Test: `tests/unit/test_database_retry.cpp`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_database_retry.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstdint>

#include "halcyon/database.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::ErrorCode;
using halcyon::ExecPolicy;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;

namespace {
halcyon::Error transientExec() {
    halcyon::Error e;
    e.code = ErrorCode::Transient;
    e.retriable = true;
    e.message = "comm lost";
    return e;
}
PoolConfig noThread() {
    PoolConfig c;
    c.startMaintenanceThread = false;
    c.backoff.sleep = [](std::chrono::milliseconds) {};
    return c;
}
MockCliDriver::ScriptedRows oneInt(std::int64_t v) {
    return MockCliDriver::ScriptedRows{{"c"}, {{halcyon::detail::cli::Value{v}}}};
}
}  // namespace

TEST(DatabaseRetry, ReadOnlyQueryRetriedOnTransientError) {
    MockCliDriver driver;
    driver.executeErrors.push_back(transientExec());  // first execute fails
    driver.resultSets.push_back(oneInt(5));           // retry succeeds
    auto db = Database::open(driver, "X", noThread()).value();

    auto qr = db.query("SELECT c FROM t");
    ASSERT_TRUE(qr.ok()) << qr.error().message;
    std::int64_t sum = 0;
    for (auto& row : qr.value()) sum += std::get<0>(row.as<std::int64_t>());
    EXPECT_EQ(sum, 5);
}

TEST(DatabaseRetry, WriteNotRetriedByDefault) {
    MockCliDriver driver;
    driver.executeErrors.push_back(transientExec());
    driver.execRowCounts.push_back(1);  // would succeed if retried
    auto db = Database::open(driver, "X", noThread()).value();

    auto n = db.execute("INSERT INTO t VALUES (?)", 1);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, ErrorCode::Transient);
}

TEST(DatabaseRetry, WriteRetriedWhenIdempotentPolicyGiven) {
    MockCliDriver driver;
    driver.executeErrors.push_back(transientExec());
    driver.execRowCounts.push_back(1);
    auto db = Database::open(driver, "X", noThread()).value();

    auto n = db.execute("INSERT INTO t VALUES (?)", ExecPolicy::idempotent(3), 1);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 1);
}

TEST(DatabaseRetry, NonRetriableErrorNotRetried) {
    MockCliDriver driver;
    halcyon::Error syntax;
    syntax.code = ErrorCode::Syntax;
    syntax.retriable = false;
    driver.executeErrors.push_back(syntax);
    driver.resultSets.push_back(oneInt(5));
    auto db = Database::open(driver, "X", noThread()).value();

    auto qr = db.query("SELECT c FROM t");
    ASSERT_FALSE(qr.ok());
    EXPECT_EQ(qr.error().code, ErrorCode::Syntax);
}

TEST(DatabaseRetry, ConnectionErrorMarksLeaseBroken) {
    MockCliDriver driver;
    halcyon::Error connErr;
    connErr.code = ErrorCode::Connection;
    connErr.retriable = true;
    driver.executeErrors.push_back(connErr);
    driver.resultSets.push_back(oneInt(9));
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    auto db = Database::open(driver, "X", cfg).value();

    auto qr = db.query("SELECT c FROM t");
    ASSERT_TRUE(qr.ok()) << qr.error().message;
    // broken lease was discarded and a replacement connected
    EXPECT_GE(driver.connectCalls, 2);
}
```

- [ ] **Step 2: Run test to verify it fails**

Add `unit/test_database_retry.cpp` to `tests/CMakeLists.txt`, build. Expected:
compile FAIL (`execute` has no `ExecPolicy` overload) and/or retry tests FAIL.

- [ ] **Step 3: Implement retry wiring**

In `include/halcyon/database.hpp`, add `#include "halcyon/retry.hpp"` and replace
the `execute`/`query`/`queryAs` bodies with retry-aware versions. Add a private
helper that runs a callable over a fresh lease per attempt, marking
connection-class failures broken:

```cpp
    // Runs op(Connection&) under a retry policy, acquiring a fresh lease each
    // attempt. A connection-class error marks the lease broken (pool discards
    // it). Read-only callers pass an idempotent policy; writes default to once().
    template <class Op>
    auto run_with_policy(const ExecPolicy& policy, Op&& op)
        -> std::invoke_result_t<Op, Connection&> {
        using R = std::invoke_result_t<Op, Connection&>;
        R last = [] { Error e; e.code = ErrorCode::Unknown;
                      e.message = "no attempt made"; return e; }();
        for (int attempt = 1; attempt <= policy.maxAttempts; ++attempt) {
            auto lease = pool_->acquire();
            if (!lease.ok()) { last = R(lease.error()); break; }
            last = op(*lease.value());
            if (last.ok()) return last;
            if (last.error().code == ErrorCode::Connection)
                lease.value().markBroken();
            if (!last.error().retriable || attempt == policy.maxAttempts) break;
            policy.backoff.sleep(policy.backoff.delay_for(attempt));
        }
        return last;
    }

    ExecPolicy read_policy() const {
        ExecPolicy p = ExecPolicy::idempotent(default_attempts_);
        p.backoff = pool_->backoff_policy();
        return p;
    }
    ExecPolicy write_policy() const {
        ExecPolicy p = ExecPolicy::once();
        p.backoff = pool_->backoff_policy();
        return p;
    }
```

Add a member `int default_attempts_ = 3;` and initialize it in the constructor
from the pool's backoff (`pool_->backoff_policy().maxAttempts`). Expose the pool's
backoff via a new accessor on `ConnectionPool` (`include/halcyon/pool.hpp`):

```cpp
    BackoffPolicy backoff_policy() const {
        std::lock_guard<std::mutex> lk(mu_);
        return config_.backoff;
    }
```

Rewrite the data-path methods to use `run_with_policy`. For `query` the
`QueryResult` must keep the *successful* lease, so it cannot use the generic
helper (which releases the lease per attempt); instead inline the retry loop and,
on success, move the lease into the `QueryResult`:

```cpp
    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::int64_t> execute(const std::string& sql, const Args&... args) {
        const ExecPolicy policy =
            detail::is_read_only(sql) ? read_policy() : write_policy();
        return run_with_policy(policy, [&](Connection& c) {
            return c.execute(sql, args...);
        });
    }

    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::int64_t> execute(const std::string& sql, const ExecPolicy& policy,
                                 const Args&... args) {
        return run_with_policy(policy, [&](Connection& c) {
            return c.execute(sql, args...);
        });
    }

    Result<std::int64_t> execute(const std::string& sql, const params& named) {
        const ExecPolicy policy =
            detail::is_read_only(sql) ? read_policy() : write_policy();
        return run_with_policy(policy, [&](Connection& c) {
            return c.execute(sql, named);
        });
    }

    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<QueryResult> query(const std::string& sql, const Args&... args) {
        const ExecPolicy policy =
            detail::is_read_only(sql) ? read_policy() : write_policy();
        return query_impl(policy, [&](Connection& c) {
            return c.query(sql, args...);
        });
    }
    Result<QueryResult> query(const std::string& sql, const params& named) {
        const ExecPolicy policy =
            detail::is_read_only(sql) ? read_policy() : write_policy();
        return query_impl(policy, [&](Connection& c) {
            return c.query(sql, named);
        });
    }

    template <class T, class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::vector<T>> queryAs(const std::string& sql, const Args&... args) {
        const ExecPolicy policy =
            detail::is_read_only(sql) ? read_policy() : write_policy();
        return run_with_policy(policy, [&](Connection& c) {
            return c.template queryAs<T>(sql, args...);
        });
    }
    template <class T>
    Result<std::vector<T>> queryAs(const std::string& sql, const params& named) {
        const ExecPolicy policy =
            detail::is_read_only(sql) ? read_policy() : write_policy();
        return run_with_policy(policy, [&](Connection& c) {
            return c.template queryAs<T>(sql, named);
        });
    }
```

with the lease-preserving query loop:

```cpp
    template <class Op>
    Result<QueryResult> query_impl(const ExecPolicy& policy, Op&& op) {
        Error last;
        last.code = ErrorCode::Unknown;
        last.message = "no attempt made";
        for (int attempt = 1; attempt <= policy.maxAttempts; ++attempt) {
            auto lease = pool_->acquire();
            if (!lease.ok()) return lease.error();
            auto rs = op(*lease.value());
            if (rs.ok())
                return QueryResult(std::move(lease.value()),
                                   std::move(rs.value()));
            last = rs.error();
            if (last.code == ErrorCode::Connection) lease.value().markBroken();
            if (!last.retriable || attempt == policy.maxAttempts) break;
            policy.backoff.sleep(policy.backoff.delay_for(attempt));
        }
        return last;
    }
```

> Note: the generic `run_with_policy` requires `R` to be constructible from
> `Error` (true for `Result<T>`). `query` cannot use it because the lease must
> survive into the returned `QueryResult`; `query_impl` handles that explicitly.

- [ ] **Step 4: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R "DatabaseRetry|Database\." --output-on-failure
```

Expected: all `DatabaseRetry.*` and the Task 3 `Database.*` tests PASS.

- [ ] **Step 5: Commit**

```bash
git add include/halcyon/database.hpp include/halcyon/pool.hpp tests/CMakeLists.txt \
        tests/unit/test_database_retry.cpp
git commit -m "feat: wire safe auto-retry (read-only + idempotent policy) into Database"
```

---

## Task 5: Functional free functions + `transaction(...)`

Adds the functional API (`query`/`execute`/`query_as`/`transaction` taking a
`Database&`) and the lease-owning `ScopedTransaction` that powers `Database::begin`
and the functional `transaction(db, fn)` / member `db.transaction(fn)` helper
(commit on success, rollback on a thrown exception or error `Result`).

**Files:**
- Modify: `include/halcyon/database.hpp` (ScopedTransaction, begin, transaction,
  free functions)
- Modify: `tests/CMakeLists.txt` (already lists test files; no change unless
  adding a new file — append the tests to `test_database.cpp`)
- Test: `tests/unit/test_database.cpp` (append functional + transaction tests)

- [ ] **Step 1: Append the failing tests**

Append to `tests/unit/test_database.cpp`:

```cpp
TEST(DatabaseTxn, CommitsOnSuccessfulLambda) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    auto r = db.transaction([](halcyon::Transaction& tx) -> halcyon::Result<int> {
        auto n = tx.execute("INSERT INTO t VALUES (?)", 1);
        if (!n.ok()) return n.error();
        return 99;
    });
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 99);
    EXPECT_EQ(driver.commitCalls, 1);
    EXPECT_EQ(driver.rollbackCalls, 0);
}

TEST(DatabaseTxn, RollsBackWhenLambdaReturnsError) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    auto r = db.transaction([](halcyon::Transaction& tx) -> halcyon::Result<int> {
        (void)tx;
        halcyon::Error e;
        e.code = halcyon::ErrorCode::Constraint;
        e.message = "dup";
        return e;
    });
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(driver.commitCalls, 0);
    EXPECT_EQ(driver.rollbackCalls, 1);
}

TEST(DatabaseTxn, RollsBackWhenLambdaThrows) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    EXPECT_THROW(
        db.transaction([](halcyon::Transaction&) -> halcyon::Result<int> {
            throw std::runtime_error("boom");
        }),
        std::runtime_error);
    EXPECT_EQ(driver.commitCalls, 0);
    EXPECT_EQ(driver.rollbackCalls, 1);
}

TEST(DatabaseTxn, BeginReturnsUsableTransaction) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    auto tx = db.begin();
    ASSERT_TRUE(tx.ok());
    ASSERT_TRUE(tx.value().execute("UPDATE t SET a=?", 1).ok());
    ASSERT_TRUE(tx.value().commit().ok());
    EXPECT_EQ(driver.commitCalls, 1);
}

TEST(FunctionalApi, FreeFunctionsDelegateToDatabase) {
    MockCliDriver driver;
    driver.resultSets.push_back(idName(3, "c"));
    driver.execRowCounts.push_back(2);
    auto db = Database::open(driver, "X", noThread()).value();

    auto n = halcyon::execute(db, "UPDATE t SET a=? WHERE id=?", 1, 3);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 2);

    auto people = halcyon::query_as<Person>(db, "SELECT id, name FROM t");
    ASSERT_TRUE(people.ok());
    ASSERT_EQ(people.value().size(), 1u);
    EXPECT_EQ(people.value()[0].id, 3);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build -j
```

Expected: compile FAIL — `db.transaction`, working `db.begin`, and the free
functions are missing.

- [ ] **Step 3: Add `ScopedTransaction`, real `begin`, `transaction`, free functions**

In `include/halcyon/database.hpp`:

First, a lease-owning transaction handle. Add **before** `class Database` (it
needs `PooledConnection` and `Transaction`, both already included):

```cpp
// A Transaction plus the pooled lease it runs on, so both are released together.
// Returned by Database::begin(); move-only. operator-> forwards to the
// Transaction. Declaration order matters: txn_ is destroyed (rolls back if not
// committed) BEFORE lease_ returns the connection to the pool.
class ScopedTransaction {
public:
    ScopedTransaction(ScopedTransaction&&) noexcept = default;
    ScopedTransaction& operator=(ScopedTransaction&&) noexcept = default;
    ScopedTransaction(const ScopedTransaction&) = delete;
    ScopedTransaction& operator=(const ScopedTransaction&) = delete;

    Transaction* operator->() noexcept { return &txn_; }
    Transaction& operator*() noexcept { return txn_; }

    bool active() const noexcept { return txn_.active(); }
    template <class... Args>
    Result<std::int64_t> execute(const std::string& sql, const Args&... a) {
        return txn_.execute(sql, a...);
    }
    Result<std::int64_t> execute(const std::string& sql, const params& n) {
        return txn_.execute(sql, n);
    }
    template <class... Args>
    Result<ResultSet> query(const std::string& sql, const Args&... a) {
        return txn_.query(sql, a...);
    }
    Result<void> commit() { return txn_.commit(); }
    Result<void> rollback() { return txn_.rollback(); }

private:
    friend class Database;
    ScopedTransaction(PooledConnection lease, Transaction txn)
        : lease_(std::move(lease)), txn_(std::move(txn)) {}

    PooledConnection lease_;  // destroyed AFTER txn_
    Transaction txn_;
};
```

> The test in Step 1 calls `db.begin()` and uses `tx.value().execute(...)` /
> `.commit()`. Change `Database::begin()` to return `Result<ScopedTransaction>`
> and update the design contract: **`Database::begin()` returns
> `Result<ScopedTransaction>`** (not a bare `Transaction`, which cannot own the
> lease). `ScopedTransaction` forwards `execute`/`query`/`commit`/`rollback`.

Replace the Task 3 temporary `begin()`/`begin_on()` with:

```cpp
    Result<ScopedTransaction> begin() {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        auto tx = lease.value()->begin();
        if (!tx.ok()) return tx.error();
        return ScopedTransaction(std::move(lease.value()),
                                 std::move(tx.value()));
    }

    // Functional transaction: commit on a successful Result, rollback on an error
    // Result or a thrown exception (then rethrow). fn must return Result<U>.
    template <class Fn>
    auto transaction(Fn&& fn) -> std::invoke_result_t<Fn, Transaction&> {
        using R = std::invoke_result_t<Fn, Transaction&>;
        auto st = begin();
        if (!st.ok()) return R(st.error());
        R r = [&]() -> R {
            try {
                return std::forward<Fn>(fn)(*st.value());
            } catch (...) {
                st.value().rollback();
                throw;
            }
        }();
        if (r.ok()) {
            auto c = st.value().commit();
            if (!c.ok()) return R(c.error());
        } else {
            st.value().rollback();
        }
        return r;
    }
```

Remove the now-unused `begin_on` declaration and its temporary definition, and
update `#include`s if needed (no new includes).

Then add the functional free functions after the `Database` class (still inside
`namespace halcyon`):

```cpp
template <class... Args>
auto query(Database& db, const std::string& sql, const Args&... args) {
    return db.query(sql, args...);
}
inline auto query(Database& db, const std::string& sql, const params& named) {
    return db.query(sql, named);
}
template <class... Args>
auto execute(Database& db, const std::string& sql, const Args&... args) {
    return db.execute(sql, args...);
}
inline auto execute(Database& db, const std::string& sql, const params& named) {
    return db.execute(sql, named);
}
template <class T, class... Args>
auto query_as(Database& db, const std::string& sql, const Args&... args) {
    return db.template queryAs<T>(sql, args...);
}
template <class T>
auto query_as(Database& db, const std::string& sql, const params& named) {
    return db.template queryAs<T>(sql, named);
}
template <class Fn>
auto transaction(Database& db, Fn&& fn) {
    return db.transaction(std::forward<Fn>(fn));
}
```

- [ ] **Step 4: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R "Database|FunctionalApi" --output-on-failure
```

Expected: all `Database.*`, `DatabaseTxn.*`, `FunctionalApi.*`, `DatabaseRetry.*`
PASS.

- [ ] **Step 5: Commit**

```bash
git add include/halcyon/database.hpp tests/unit/test_database.cpp
git commit -m "feat: add functional API and transaction(fn) with ScopedTransaction"
```

---

## Task 6: Bulk / batch path (`executeBatch` / `batchOf`)

Adds a batch type and `Database::executeBatch(sql, batch)` that prepares once and
executes the statement per row (v1: a portable per-row loop inside one acquired
connection; array-insert optimization is a future enhancement behind the same
API). `batchOf` builds a `Batch` from a vector of reflected structs or from
explicit value rows.

**Files:**
- Modify: `include/halcyon/database.hpp` (Batch, batchOf, executeBatch)
- Modify: `include/halcyon/connection.hpp` (add `Connection::executeBatch`)
- Modify: `tests/CMakeLists.txt` (add `unit/test_database_batch.cpp`)
- Test: `tests/unit/test_database_batch.cpp`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_database_batch.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "halcyon/database.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;

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

TEST(Batch, ExecutesOncePerRowReturningTotal) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    driver.execRowCounts.push_back(1);
    driver.execRowCounts.push_back(1);
    auto db = Database::open(driver, "X", noThread()).value();

    auto batch = halcyon::batchOf<Pair>({{1, "a"}, {2, "b"}, {3, "c"}});
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 3);
    // one prepare reused across rows
    EXPECT_EQ(driver.preparedSql.size(), 1u);
}

TEST(Batch, EmptyBatchIsNoOp) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)",
                             halcyon::Batch{});
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 0);
    EXPECT_EQ(driver.preparedSql.size(), 0u);
}

TEST(Batch, StopsAndReturnsErrorOnRowFailure) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    halcyon::Error e;
    e.code = halcyon::ErrorCode::Constraint;
    e.message = "dup";
    driver.executeErrors.push_back(e);  // second row fails (after 1st count used)
    auto db = Database::open(driver, "X", noThread()).value();

    auto batch = halcyon::batchOf<Pair>({{1, "a"}, {2, "b"}});
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, halcyon::ErrorCode::Constraint);
}
```

> Note on MockCliDriver ordering: `execute()` consumes `executeErrors` first, then
> `resultSets`, then `execRowCounts`. In `StopsAndReturnsErrorOnRowFailure` the
> first row pulls `execRowCounts.front()` (1) only if no error is queued; since the
> single queued error is consumed by the *first* execute, push a success first.
> Adjust the test so the error lands on the intended row: push `execRowCounts` for
> row 1 and the error such that row 1 succeeds and row 2 fails — with the mock's
> "errors first" rule, queue the error to fire on row 2 by leaving row 1 to consume
> the row count. Because errors are always consumed first, this test instead
> expects the FIRST row to fail; rename the rows accordingly and assert the batch
> stops with 0 prior successes. (Keep it simple: the error fires on row 1.)

Replace the body of `StopsAndReturnsErrorOnRowFailure` with the mock-accurate
version:

```cpp
TEST(Batch, StopsAndReturnsErrorOnRowFailure) {
    MockCliDriver driver;
    halcyon::Error e;
    e.code = halcyon::ErrorCode::Constraint;
    e.message = "dup";
    driver.executeErrors.push_back(e);  // first execute fails
    auto db = Database::open(driver, "X", noThread()).value();

    auto batch = halcyon::batchOf<Pair>({{1, "a"}, {2, "b"}});
    auto n = db.executeBatch("INSERT INTO t(a,b) VALUES (?,?)", batch);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, halcyon::ErrorCode::Constraint);
}
```

- [ ] **Step 2: Run test to verify it fails**

Add `unit/test_database_batch.cpp` to `tests/CMakeLists.txt`, build. Expected:
compile FAIL — `Batch`, `batchOf`, `executeBatch` missing.

- [ ] **Step 3: Add the batch type + `Connection::executeBatch`**

In `include/halcyon/connection.hpp`, add a method to `Connection` that prepares
once and binds/executes per row (after the `queryAs` overloads):

```cpp
    // Prepares sql once and executes it for each row of positional params,
    // accumulating affected-row counts. Stops at the first error.
    Result<std::int64_t> executeBatch(
        const std::string& sql,
        const std::vector<std::vector<detail::cli::Value>>& rows) {
        if (rows.empty()) return std::int64_t{0};
        auto st = prepare(sql);
        if (!st.ok()) return st.error();
        std::int64_t total = 0;
        for (const auto& row : rows) {
            auto b = driver_->bindParams(st.value().handle(), row);
            if (!b.ok()) return b.error();
            auto e = driver_->execute(st.value().handle());
            if (!e.ok()) return e.error();
            total += e.value();
        }
        return total;
    }
```

In `include/halcyon/database.hpp`, add the `Batch` type, `batchOf` overloads, and
`Database::executeBatch` (place `Batch`/`batchOf` before `class Database`, and the
method inside it):

```cpp
// A prepared set of positional parameter rows for executeBatch.
struct Batch {
    std::vector<std::vector<detail::cli::Value>> rows;
};

// batchOf for a vector of HALCYON_REFLECT'd structs: each field becomes a
// positional bind in declaration order.
template <class T>
Batch batchOf(const std::vector<T>& items) {
    static_assert(reflect::Reflected<T>::value,
                  "batchOf<T> requires HALCYON_REFLECT(T, fields...)");
    Batch out;
    out.rows.reserve(items.size());
    auto mptrs = reflect::Reflected<T>::members();
    constexpr std::size_t N = reflect::Reflected<T>::field_count;
    for (const auto& item : items) {
        std::vector<detail::cli::Value> row;
        row.reserve(N);
        detail::batch_append_fields(row, item, mptrs,
                                    std::make_index_sequence<N>{});
        out.rows.push_back(std::move(row));
    }
    return out;
}

// batchOf for an initializer list of reflected structs: batchOf<T>({{...},{...}}).
template <class T>
Batch batchOf(std::initializer_list<T> items) {
    return batchOf(std::vector<T>(items));
}
```

and the field-append helper in `namespace detail` (top of `database.hpp`, after
includes):

```cpp
namespace detail {
template <class T, class Tuple, std::size_t... I>
void batch_append_fields(std::vector<detail::cli::Value>& row, const T& item,
                         const Tuple& mptrs, std::index_sequence<I...>) {
    (row.push_back(detail::to_value(item.*std::get<I>(mptrs))), ...);
}
}  // namespace detail
```

Add the method inside `Database` (no retry: batch is a write path; a connection
error returns to the caller, who re-drives the batch):

```cpp
    Result<std::int64_t> executeBatch(const std::string& sql, const Batch& batch) {
        auto lease = pool_->acquire();
        if (!lease.ok()) return lease.error();
        return lease.value()->executeBatch(sql, batch.rows);
    }
```

Add `#include <initializer_list>` to `database.hpp`.

- [ ] **Step 4: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R "Batch" --output-on-failure
```

Expected: all `Batch.*` PASS.

- [ ] **Step 5: Commit**

```bash
git add include/halcyon/database.hpp include/halcyon/connection.hpp \
        tests/CMakeLists.txt tests/unit/test_database_batch.cpp
git commit -m "feat: add executeBatch + batchOf bulk insert path"
```

---

## Task 7: Async on `Database` (`queryAsync` / `executeAsync`)

Adds async overloads backed by an internal `Executor` (Plan 3) owned by the
`Database` (shared with the pool's lifetime). `queryAsync<T>` materializes rows
(returns `std::future<Result<std::vector<T>>>`) so the future has no lease-lifetime
concern; `executeAsync` returns `std::future<Result<std::int64_t>>`.

**Files:**
- Modify: `include/halcyon/database.hpp` (own an Executor; async methods)
- Modify: `tests/CMakeLists.txt` (add `unit/test_database_async.cpp`)
- Test: `tests/unit/test_database_async.cpp`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_database_async.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstdint>
#include <future>
#include <string>
#include <vector>

#include "halcyon/database.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;

namespace {
PoolConfig noThread() {
    PoolConfig c;
    c.startMaintenanceThread = false;
    return c;
}
}  // namespace

struct Num {
    std::int64_t n;
};
HALCYON_REFLECT(Num, n);

TEST(DatabaseAsync, ExecuteAsyncReturnsCount) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(4);
    auto db = Database::open(driver, "X", noThread()).value();
    auto f = db.executeAsync("UPDATE t SET a=? WHERE id=?", 1, 2);
    auto r = f.get();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 4);
}

TEST(DatabaseAsync, QueryAsAsyncMaterializesRows) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"n"},
        {{halcyon::detail::cli::Value{std::int64_t{2}}},
         {halcyon::detail::cli::Value{std::int64_t{5}}}}});
    auto db = Database::open(driver, "X", noThread()).value();
    auto f = db.queryAsAsync<Num>("SELECT n FROM t");
    auto r = f.get();
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().size(), 2u);
    EXPECT_EQ(r.value()[1].n, 5);
}
```

- [ ] **Step 2: Run test to verify it fails**

Add `unit/test_database_async.cpp` to `tests/CMakeLists.txt`, build. Expected:
compile FAIL — no async methods.

- [ ] **Step 3: Own an `Executor` and add async methods**

In `include/halcyon/database.hpp`, add `#include "halcyon/async.hpp"` and
`#include <future>`. Give `Database` a shared executor so copies share it:

```cpp
    std::shared_ptr<Executor> exec_;
```

Initialize it in `open` (size = pool max, min 1). Change the private constructor
to take both:

```cpp
    explicit Database(std::shared_ptr<ConnectionPool> pool,
                      std::shared_ptr<Executor> exec)
        : pool_(std::move(pool)), exec_(std::move(exec)) {
        default_attempts_ = pool_->backoff_policy().maxAttempts;
    }
```

and in `open`:

```cpp
    static Result<Database> open(detail::cli::ICliDriver& driver,
                                 const std::string& dsn, PoolConfig config = {}) {
        const std::size_t threads = config.max ? config.max : 1;
        auto pool = ConnectionPool::create(driver, {dsn}, std::move(config));
        if (!pool.ok()) return pool.error();
        return Database(
            std::shared_ptr<ConnectionPool>(std::move(pool.value())),
            std::make_shared<Executor>(threads));
    }
```

Add async methods inside `Database` (capture `this`; the shared pool/exec keep the
backing alive as long as any copy lives — document that futures must complete
before the last `Database` copy is destroyed):

```cpp
    template <class... Args>
    std::future<Result<std::int64_t>> executeAsync(const std::string& sql,
                                                    Args... args) {
        return exec_->submit([this, sql, args...]() {
            return execute(sql, args...);
        });
    }

    template <class T, class... Args>
    std::future<Result<std::vector<T>>> queryAsAsync(const std::string& sql,
                                                      Args... args) {
        return exec_->submit([this, sql, args...]() {
            return this->template queryAs<T>(sql, args...);
        });
    }
```

> Note: a raw `queryAsync` returning `QueryResult` across threads would tie a pool
> lease to a future; per the spec's streaming non-goal, the async surface
> materializes (`queryAsAsync<T>`) and offers `executeAsync`. Streaming-async is a
> documented future extension.

- [ ] **Step 4: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R "DatabaseAsync" --output-on-failure
```

Expected: `DatabaseAsync.*` PASS.

- [ ] **Step 5: Commit**

```bash
git add include/halcyon/database.hpp tests/CMakeLists.txt \
        tests/unit/test_database_async.cpp
git commit -m "feat: add executeAsync/queryAsAsync backed by shared Executor"
```

---

## Task 8: Umbrella header, examples, and gated integration tests

Exposes the new headers via the umbrella, adds OO + functional examples (built
under `HALCYON_BUILD_EXAMPLES`), and adds live-DB facade/transaction integration
tests (skipped without `HALCYON_TEST_DSN`).

**Files:**
- Modify: `include/halcyon/halcyon.hpp`
- Modify: `CMakeLists.txt` (add examples subdir)
- Create: `examples/CMakeLists.txt`, `examples/oo_usage.cpp`,
  `examples/functional_usage.cpp`
- Modify: `tests/integration/test_db2_roundtrip.cpp`

- [ ] **Step 1: Extend the umbrella header**

Edit `include/halcyon/halcyon.hpp` to add the facade headers (keep sorted):

```cpp
#pragma once

// Halcyon — modern C++17 IBM Db2 client. Umbrella header.
#include "halcyon/async.hpp"
#include "halcyon/connection.hpp"
#include "halcyon/database.hpp"
#include "halcyon/error.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/pool.hpp"
#include "halcyon/result.hpp"
#include "halcyon/retry.hpp"
#include "halcyon/transaction.hpp"
#include "halcyon/types.hpp"
#include "halcyon/version.hpp"
#include "halcyon/detail/cli/db2_cli_driver.hpp"
```

- [ ] **Step 2: Build the unit suite (umbrella compiles)**

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Expected: every unit test from Plans 1–4 PASSES.

- [ ] **Step 3: Add examples**

Create `examples/oo_usage.cpp`:

```cpp
// OO/throwing style. Requires a live DSN in HALCYON_TEST_DSN to actually run;
// otherwise it prints usage and exits 0 so the build stays self-contained.
#include <cstdlib>
#include <iostream>

#include "halcyon/halcyon.hpp"

struct User {
    std::int64_t id;
    std::string name;
};
HALCYON_REFLECT(User, id, name);

int main() {
    const char* dsn = std::getenv("HALCYON_TEST_DSN");
    if (!dsn) {
        std::cout << "Set HALCYON_TEST_DSN to run this example.\n";
        return 0;
    }
    auto driver = halcyon::detail::cli::make_db2_cli_driver();
    try {
        auto db = halcyon::Database::openOrThrow(*driver, dsn,
                                                 halcyon::PoolConfig{});
        auto n = db.executeOrThrow(
            "SELECT 1 FROM SYSIBM.SYSDUMMY1");
        (void)n;
        auto rows = db.queryOrThrow("SELECT 1 FROM SYSIBM.SYSDUMMY1");
        for (auto& row : rows) {
            std::cout << "value=" << std::get<0>(row.as<std::int64_t>()) << "\n";
        }
    } catch (const halcyon::Exception& e) {
        std::cerr << "halcyon error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
```

Create `examples/functional_usage.cpp`:

```cpp
// Functional/Result style with a transaction.
#include <cstdlib>
#include <iostream>

#include "halcyon/halcyon.hpp"

int main() {
    const char* dsn = std::getenv("HALCYON_TEST_DSN");
    if (!dsn) {
        std::cout << "Set HALCYON_TEST_DSN to run this example.\n";
        return 0;
    }
    auto driver = halcyon::detail::cli::make_db2_cli_driver();
    auto db = halcyon::Database::open(*driver, dsn);
    if (!db.ok()) {
        std::cerr << "open failed: " << db.error().message << "\n";
        return 1;
    }
    auto r = halcyon::transaction(
        db.value(), [](halcyon::Transaction& tx) -> halcyon::Result<std::int64_t> {
            return tx.execute("SELECT 1 FROM SYSIBM.SYSDUMMY1");
        });
    if (!r.ok()) {
        std::cerr << "txn failed: " << r.error().message << "\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
```

Create `examples/CMakeLists.txt`:

```cmake
add_executable(halcyon_example_oo oo_usage.cpp)
target_link_libraries(halcyon_example_oo PRIVATE halcyon::halcyon)

add_executable(halcyon_example_functional functional_usage.cpp)
target_link_libraries(halcyon_example_functional PRIVATE halcyon::halcyon)
```

In the root `CMakeLists.txt`, add (after the tests block):

```cmake
if(HALCYON_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
```

- [ ] **Step 4: Add gated facade integration tests**

Append to `tests/integration/test_db2_roundtrip.cpp`:

```cpp
TEST(Db2FacadeIntegration, QueryExecuteAndTransaction) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";

    auto driver = halcyon::detail::cli::make_db2_cli_driver();
    auto db = halcyon::Database::open(*driver, *d);
    ASSERT_TRUE(db.ok()) << db.error().message;

    auto qr = db.value().query("SELECT 1 FROM SYSIBM.SYSDUMMY1");
    ASSERT_TRUE(qr.ok()) << qr.error().message;
    std::int64_t sum = 0;
    for (auto& row : qr.value()) sum += std::get<0>(row.as<std::int64_t>());
    EXPECT_EQ(sum, 1);

    auto txr = halcyon::transaction(
        db.value(),
        [](halcyon::Transaction& tx) -> halcyon::Result<std::int64_t> {
            return tx.execute("SELECT 1 FROM SYSIBM.SYSDUMMY1");
        });
    ASSERT_TRUE(txr.ok()) << txr.error().message;
}
```

- [ ] **Step 5: Build everything (no DB needed to compile)**

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON \
    -DHALCYON_BUILD_INTEGRATION_TESTS=ON -DHALCYON_BUILD_EXAMPLES=ON
cmake --build build -j
```

Expected: unit + integration + example targets build. Facade integration test
reports **skipped** without `HALCYON_TEST_DSN`.

- [ ] **Step 6: Run the full unit suite**

```bash
ctest --test-dir build --output-on-failure
```

Expected: every unit test from Plans 1–4 PASSES.

- [ ] **Step 7: Run integration against live Db2 (REQUIRED at verification)**

```bash
docker compose -f docker/docker-compose.yml up -d
docker compose -f docker/docker-compose.yml ps   # wait for STATUS = healthy
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
ctest --test-dir build -L integration --output-on-failure
docker compose -f docker/docker-compose.yml down
```

Expected: `Db2Integration.*`, `Db2PoolIntegration.*`, and
`Db2FacadeIntegration.*` PASS against live Db2 (not skipped).

- [ ] **Step 8: Commit**

```bash
git add include/halcyon/halcyon.hpp CMakeLists.txt examples/ \
        tests/integration/test_db2_roundtrip.cpp
git commit -m "test: expose facade via umbrella, add examples and gated facade integration tests"
```

---

## Self-Review

**Spec coverage (Plan 4 scope, spec §5 + §8 retry/async + §6 dual errors):**
- `Database::open`/`openOrThrow` over a pool → Task 3. ✔
- OO `query` (anon + named), `execute`, `queryAs` with dual `Result`/throwing
  overloads → Task 3. ✔ (`QueryResult` keeps the lease alive for cursor range
  iteration — the spec's `for (auto& row : rows.value())` works.)
- Functional free functions (`query`/`execute`/`query_as`/`transaction`) → Task 5. ✔
- RAII `Transaction` (`begin`/`commit`/auto-rollback) + functional `transaction(fn)`
  (commit on success, rollback on error/throw) → Tasks 2 + 5. ✔
- Safe auto-retry: read-only auto-retried, writes opt-in via `ExecPolicy`,
  connection-class error marks lease broken → Task 4. ✔ (Transactions are never
  auto-retried — `Transaction` does not wrap calls in `with_retry`.)
- Bulk/batch (`executeBatch`/`batchOf`) → Task 6. ✔ (per-row loop, single prepare;
  array-insert is a documented future optimization behind the same API.)
- Async `std::future` (`executeAsync`/`queryAsAsync`) on the Plan 3 `Executor` →
  Task 7. ✔
- Seam stays the only `sqlcli1.h` user; transaction control added under
  `detail::cli` (`Db2CliDriver`) and mocked → Task 1. ✔
- Thread-safety: `Database` is a shared-pool handle; per-call lease ensures one
  thread per `Connection`; `Transaction`/`QueryResult` are single-owner. ✔

**Type consistency:** `QueryResult`/`ScopedTransaction` declare the
`PooledConnection` lease **before** the `ResultSet`/`Transaction` so the lease is
released last (rollback/cursor-close happen before the connection returns to the
pool). `Database::begin()` returns `Result<ScopedTransaction>` (finalized in Task
5; the design contract is updated accordingly — Task 3 ships a temporary stub that
Task 5 replaces). `ExecPolicy`/`BackoffPolicy`/`with_retry`/`is_read_only` (Plan 3)
drive Task 4; `ConnectionPool::backoff_policy()` is the new accessor. `Connection`
gains `driver()` (Task 2) and `executeBatch` (Task 6). `Transaction` forwards to
`Connection::execute`/`query`/`queryAs` (Plan 2 signatures, unchanged).

**Placeholder scan:** No TBD/TODO left in shipped code. The single intentional
temporary is `Database::begin_on` in Task 3 (returns a "not yet implemented" error
and is **not** tested in Task 3); Task 5 deletes it and ships the real
`ScopedTransaction`-based `begin()`. Every code step contains complete code.

**Determinism:** Retry tests inject a no-op `backoff.sleep` (no real waiting).
Async tests `.get()` the futures (deterministic). No test sleeps for real time.

**Deferred (intentionally):**
- Array/multi-row server-side batch insert (the `executeBatch` API is stable; the
  internal loop can be swapped for `SQLBindParameter` arrays later).
- Streaming async (`queryAsync` returning a live cursor across threads).
- Observability emission at facade boundaries → Plan 5.
- Per-connection prepared-statement cache (spec non-goal for v1).

---

## Roadmap: subsequent plan

- **Plan 5 — Observability:** `MetricsSink`/`Tracer` no-op defaults, `prometheus-cpp`
  and `opentelemetry-cpp` adapters behind `HALCYON_WITH_PROMETHEUS`/`HALCYON_WITH_OTEL`,
  metric/span emission at query/transaction/pool boundaries (`halcyon_queries_total`,
  `halcyon_query_duration_seconds`, `halcyon_pool_connections`,
  `halcyon_reconnects_total`, `halcyon_retries_total`, `halcyon.query`/
  `halcyon.transaction` spans).
