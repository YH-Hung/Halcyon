# Per-Connection Prepared-Statement LRU Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Add a transparent, per-connection fixed-capacity LRU cache of prepared
statement handles so repeated SQL on a pooled connection skips re-`prepare`,
while keeping behavior and error semantics identical to today.

**Architecture:** A new `detail::StatementCache` (core, above the CLI seam) owns
live `StatementHandle`s keyed by the exact prepared SQL text, lending them via an
RAII `StatementLease`. `Connection` owns one cache (via `unique_ptr`, stable
address) and routes `query`/`execute`/`queryAs`/`executeBatch` through it instead
of preparing directly; `ResultSet` owns the lease for its cursor's lifetime. One
portable seam method, `ICliDriver::closeCursor`, is added so a cached statement's
cursor can be reset for reuse. Capacity comes from `PoolConfig.statementCacheSize`
(default 64; the pool injects it, so the `Database` product path is cached by
default). Direct `Connection` construction defaults to capacity 0 (disabled),
preserving the existing low-level prepare/finalize contract.

**Tech Stack:** C++17, CMake ≥ 3.20, GoogleTest (existing harness), the existing
`MockCliDriver` seam fake, the vendored IBM Db2 CLI driver for the gated
integration suite.

**Spec:** `docs/superpowers/specs/2026-06-21-halcyon-stmt-cache-design.md`

**Builds on:** `include/halcyon/detail/cli/driver.hpp` (seam),
`include/halcyon/connection.hpp` (`Connection`/`Statement`/`ResultSet`),
`include/halcyon/pool.hpp` (`PoolConfig`/`ConnectionPool`),
`include/halcyon/observability/metrics.hpp` (`obs::MetricsSink`),
`tests/unit/mock_cli_driver.hpp`.

---

## Design contracts (shared across tasks — keep consistent)

These names/signatures are referenced by multiple tasks. Treat them as the source
of truth; if a later task needs a change, update this section in the same commit.

**Seam addition (Task 1, `detail/cli/driver.hpp`):**

```cpp
// Closes any open cursor on stmt and resets it for re-bind/re-execute.
// Idempotent when no cursor is open. (SQLFreeStmt(SQL_CLOSE) in the real driver.)
virtual Result<void> closeCursor(StatementHandle stmt) = 0;
```

**Cache entry + lease (Task 2, `detail/statement_cache.hpp`, namespace `halcyon::detail`):**

```cpp
struct StmtCacheEntry {
    std::string key;
    cli::StatementHandle handle = cli::StatementHandle::invalid;
    bool busy = false;
};

class StatementCache;  // owns entries; non-movable; address stable via unique_ptr

class StatementLease {                 // RAII borrow; move-only
  public:
    StatementLease() = default;        // empty/null lease
    cli::StatementHandle handle() const noexcept;
    void poison() noexcept;            // drop (finalize) on release after an error
    // move ctor/assign null out the source; dtor releases.
};
```

**Cache API (Task 2):**

```cpp
StatementCache(cli::ICliDriver& driver, cli::ConnectionHandle conn,
               std::size_t capacity, obs::MetricsSink* metrics = nullptr);
Result<StatementLease> acquire(const std::string& sql);
// capacity 0 → always transient. Hit→reuse, miss→prepare+insert (+evict LRU idle),
// hit-but-busy or all-busy-at-capacity → transient (overflow).
```

**Connection wiring (Task 3, `connection.hpp`):**

```cpp
Connection(detail::cli::ICliDriver& driver, detail::cli::ConnectionHandle handle,
           std::size_t statementCacheSize = 0, obs::MetricsSink* metrics = nullptr);
static Result<Connection> open(detail::cli::ICliDriver& driver,
                               const detail::cli::ConnectionParams& params,
                               std::size_t statementCacheSize = 0,
                               obs::MetricsSink* metrics = nullptr);
```

`ResultSet` owns `std::optional<detail::StatementLease> lease_` (replacing
`std::optional<Statement> owned_`).

**Pool wiring (Task 4, `pool.hpp`):** `PoolConfig` gains
`std::size_t statementCacheSize = 64;`. `make_connection()` passes it plus the
pool's resolved metrics sink into `Connection::open`.

**Metrics (Task 2, via `obs::MetricsSink`):**

- `halcyon_stmt_cache_total{result="hit"|"miss"|"evict"|"overflow"}` — counter
- `halcyon_stmt_cache_size` — gauge

---

## File Structure (created/modified by this plan)

- `include/halcyon/detail/cli/driver.hpp` — **modify**: add `closeCursor` pure virtual.
- `src/detail/cli/db2_cli_driver.cpp` — **modify**: implement `closeCursor`.
- `tests/unit/mock_cli_driver.hpp` — **modify**: implement `closeCursor`, add counters.
- `tests/unit/test_cli_seam.cpp` — **modify**: a `closeCursor` seam test.
- `include/halcyon/detail/statement_cache.hpp` — **create**: `StmtCacheEntry`, `StatementLease`, `StatementCache`.
- `tests/unit/test_statement_cache.cpp` — **create**: cache unit tests.
- `include/halcyon/connection.hpp` — **modify**: cache member, lease-owning `ResultSet`, routed methods, new ctor/open params.
- `tests/unit/test_connection.cpp` — **modify**: caching-on reuse + overflow + poison tests.
- `include/halcyon/pool.hpp` — **modify**: `PoolConfig.statementCacheSize`, inject into `make_connection`.
- `tests/unit/test_pool.cpp` — **modify**: pool reuse + reconnect-fresh-cache tests.
- `tests/CMakeLists.txt` — **modify**: register `test_statement_cache.cpp`.
- `tests/integration/test_db2_roundtrip.cpp` — **modify**: gated reuse + cursor-reuse correctness.

---

## Task 1: Add `closeCursor` to the CLI seam

**Files:**
- Modify: `include/halcyon/detail/cli/driver.hpp`
- Modify: `tests/unit/mock_cli_driver.hpp`
- Modify: `src/detail/cli/db2_cli_driver.cpp`
- Modify: `tests/unit/test_cli_seam.cpp`

- [x] **Step 1: Write the failing seam test**

Append to `tests/unit/test_cli_seam.cpp`:

```cpp
TEST(CliSeamStatement, CloseCursorIsCallableAndIdempotent) {
    MockCliDriver driver;
    auto conn = driver.connect(ConnectionParams{"x"}).value();
    auto st = driver.prepare(conn, "SELECT id FROM t").value();

    auto a = driver.closeCursor(st);
    auto b = driver.closeCursor(st);  // idempotent: no open cursor

    EXPECT_TRUE(a.ok());
    EXPECT_TRUE(b.ok());
    EXPECT_EQ(driver.closeCursorCalls, 2);
}
```

- [x] **Step 2: Run the test to verify it fails to compile**

Run: `cmake --build build --target halcyon_unit_tests -j`
Expected: FAIL — `closeCursor` is not a member of `MockCliDriver` /
`closeCursorCalls` undefined.

- [x] **Step 3: Add the pure virtual to the seam**

In `include/halcyon/detail/cli/driver.hpp`, add after the `finalize` declaration
(after line 73, inside the `// --- Prepared-statement data path` group):

```cpp
    // Closes any open cursor on stmt and resets it so it can be re-bound and
    // re-executed. Idempotent when no cursor is open. Required to reuse a cached
    // prepared statement that previously produced a result set.
    virtual Result<void> closeCursor(StatementHandle stmt) = 0;
```

- [x] **Step 4: Implement it in the mock**

In `tests/unit/mock_cli_driver.hpp`, add a counter near the other statement
counters (after the `finalized` vector around line 178):

```cpp
    int closeCursorCalls = 0;
```

And add the method after `finalize` (after line 175):

```cpp
    Result<void> closeCursor(StatementHandle stmt) override {
        ++closeCursorCalls;
        auto it = statements.find(stmt);
        if (it != statements.end()) it->second.position = -1;  // cursor reset
        return Result<void>();
    }
```

- [x] **Step 5: Implement it in the real driver**

In `src/detail/cli/db2_cli_driver.cpp`, add after `finalize` (after line 331):

```cpp
    Result<void> closeCursor(StatementHandle stmt) override {
        StmtState* st = stmt_state(stmt);
        if (!st) return Result<void>();  // unknown/finalized handle: no-op
        SQLRETURN rc = SQLFreeStmt(st->handle, SQL_CLOSE);
        // SQL_SUCCESS_WITH_INFO / "no cursor open" are benign; only a hard error
        // is reported. cli_ok() accepts SUCCESS and SUCCESS_WITH_INFO.
        if (!cli_ok(rc))
            return make_error(SQL_HANDLE_STMT, st->handle, ErrorCode::Unknown,
                              "SQLFreeStmt(SQL_CLOSE) failed");
        return Result<void>();
    }
```

- [x] **Step 6: Run the seam test to verify it passes**

Run: `cmake --build build --target halcyon_unit_tests -j && ctest --test-dir build -R CliSeamStatement --output-on-failure`
Expected: PASS.

- [x] **Step 7: Commit**

```bash
git add include/halcyon/detail/cli/driver.hpp tests/unit/mock_cli_driver.hpp \
        src/detail/cli/db2_cli_driver.cpp tests/unit/test_cli_seam.cpp
git commit -m "feat: add ICliDriver::closeCursor seam op for statement reuse"
```

---

## Task 2: Create `StatementCache` and `StatementLease`

**Files:**
- Create: `include/halcyon/detail/statement_cache.hpp`
- Create: `tests/unit/test_statement_cache.cpp`
- Modify: `tests/CMakeLists.txt`

- [x] **Step 1: Register the new unit test**

In `tests/CMakeLists.txt`, add to the `add_executable(halcyon_unit_tests ...)`
list (after `unit/test_connection.cpp` on line 19):

```cmake
    unit/test_statement_cache.cpp
```

- [x] **Step 2: Write the failing cache tests**

Create `tests/unit/test_statement_cache.cpp`:

```cpp
#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "halcyon/detail/statement_cache.hpp"
#include "mock_cli_driver.hpp"

using halcyon::detail::StatementCache;
using halcyon::detail::StatementLease;
using halcyon::testing::MockCliDriver;
using ConnectionParams = halcyon::detail::cli::ConnectionParams;

namespace {

// Minimal recording sink: counts counter samples by (name, result-label) and
// remembers the last gauge value per name.
class RecordingSink : public halcyon::obs::MetricsSink {
  public:
    void counter(std::string_view n, double v,
                 const halcyon::obs::Labels& l) override {
        std::string key{n};
        for (auto& kv : l)
            if (kv.first == "result") key += std::string{":"} + std::string{kv.second};
        counts[key] += v;
    }
    void histogram(std::string_view, double,
                   const halcyon::obs::Labels&) override {}
    void gauge(std::string_view n, double v,
               const halcyon::obs::Labels&) override {
        gauges[std::string{n}] = v;
    }
    std::map<std::string, double> counts;
    std::map<std::string, double> gauges;
};

inline halcyon::detail::cli::ConnectionHandle open_conn(MockCliDriver& d) {
    return d.connect(ConnectionParams{"x"}).value();
}

}  // namespace

TEST(StatementCache, MissThenHitPreparesOnce) {
    MockCliDriver driver;
    StatementCache cache(driver, open_conn(driver), /*capacity=*/8);
    const std::string sql = "SELECT 1 FROM SYSIBM.SYSDUMMY1";

    { auto l1 = cache.acquire(sql); ASSERT_TRUE(l1.ok()); }  // miss, released
    { auto l2 = cache.acquire(sql); ASSERT_TRUE(l2.ok()); }  // hit, released

    EXPECT_EQ(driver.preparedSql.size(), 1u);  // prepared once, reused
    EXPECT_EQ(driver.finalizeCalls, 0);        // cached, not finalized
    EXPECT_GE(driver.closeCursorCalls, 2);     // closed on each release
}

TEST(StatementCache, CapacityZeroIsAlwaysTransient) {
    MockCliDriver driver;
    StatementCache cache(driver, open_conn(driver), /*capacity=*/0);
    const std::string sql = "SELECT 1";

    { auto l = cache.acquire(sql); ASSERT_TRUE(l.ok()); }
    { auto l = cache.acquire(sql); ASSERT_TRUE(l.ok()); }

    EXPECT_EQ(driver.preparedSql.size(), 2u);  // no reuse
    EXPECT_EQ(driver.finalizeCalls, 2);        // each transient finalized
}

TEST(StatementCache, EvictsLeastRecentlyUsedAtCapacity) {
    MockCliDriver driver;
    StatementCache cache(driver, open_conn(driver), /*capacity=*/2);

    { auto a = cache.acquire("A"); }  // entries: A
    { auto b = cache.acquire("B"); }  // entries: B, A
    { auto a = cache.acquire("A"); }  // touch A -> entries: A, B
    { auto c = cache.acquire("C"); }  // capacity hit: evict LRU idle = B

    EXPECT_EQ(driver.preparedSql.size(), 3u);  // A, B, C
    EXPECT_EQ(driver.finalizeCalls, 1);        // B evicted+finalized
    { auto b = cache.acquire("B"); }           // B was evicted -> re-prepare
    EXPECT_EQ(driver.preparedSql.size(), 4u);
}

TEST(StatementCache, BusyHitServesTransientOverflow) {
    MockCliDriver driver;
    StatementCache cache(driver, open_conn(driver), /*capacity=*/8);
    const std::string sql = "SELECT id FROM t";

    auto l1 = cache.acquire(sql);  // miss, kept busy (not released)
    ASSERT_TRUE(l1.ok());
    auto l2 = cache.acquire(sql);  // same sql while busy -> transient
    ASSERT_TRUE(l2.ok());

    EXPECT_EQ(driver.preparedSql.size(), 2u);              // one cached + one transient
    EXPECT_NE(l1.value().handle(), l2.value().handle());   // distinct handles
}

TEST(StatementCache, PoisonDropsEntry) {
    MockCliDriver driver;
    StatementCache cache(driver, open_conn(driver), /*capacity=*/8);
    const std::string sql = "SELECT 1";

    { auto l = cache.acquire(sql); ASSERT_TRUE(l.ok()); l.value().poison(); }
    EXPECT_EQ(driver.finalizeCalls, 1);  // poisoned entry finalized on release
    { auto l = cache.acquire(sql); ASSERT_TRUE(l.ok()); }  // re-prepared
    EXPECT_EQ(driver.preparedSql.size(), 2u);
}

TEST(StatementCache, DestructorFinalizesLiveEntries) {
    MockCliDriver driver;
    {
        StatementCache cache(driver, open_conn(driver), /*capacity=*/8);
        { auto a = cache.acquire("A"); }
        { auto b = cache.acquire("B"); }
        EXPECT_EQ(driver.finalizeCalls, 0);
    }  // cache destroyed
    EXPECT_EQ(driver.finalizeCalls, 2);  // A and B finalized
}

TEST(StatementCache, EmitsMetrics) {
    MockCliDriver driver;
    RecordingSink sink;
    StatementCache cache(driver, open_conn(driver), /*capacity=*/8, &sink);
    const std::string sql = "SELECT 1";

    { auto a = cache.acquire(sql); }  // miss
    { auto b = cache.acquire(sql); }  // hit

    EXPECT_DOUBLE_EQ(sink.counts["halcyon_stmt_cache_total:miss"], 1.0);
    EXPECT_DOUBLE_EQ(sink.counts["halcyon_stmt_cache_total:hit"], 1.0);
    EXPECT_DOUBLE_EQ(sink.gauges["halcyon_stmt_cache_size"], 1.0);
}
```

- [x] **Step 3: Run the tests to verify they fail**

Run: `cmake --build build --target halcyon_unit_tests -j`
Expected: FAIL — `halcyon/detail/statement_cache.hpp` not found.

- [x] **Step 4: Create the cache header**

Create `include/halcyon/detail/statement_cache.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/observability/metrics.hpp"
#include "halcyon/result.hpp"

namespace halcyon::detail {

// One cached prepared statement. Address-stable inside the owning std::list, so a
// StatementLease may hold a raw pointer to it for its (short) lifetime.
struct StmtCacheEntry {
    std::string key;
    cli::StatementHandle handle = cli::StatementHandle::invalid;
    bool busy = false;
};

class StatementCache;  // fwd

// RAII borrow of a prepared statement handle. Move-only. Two modes:
//  - cached: references a StatementCache entry; on release the cursor is closed
//    and the entry returns to the LRU as most-recently-used (or, if poisoned, it
//    is finalized and dropped).
//  - transient: owns the handle outright; on release it is finalized.
class StatementLease {
  public:
    StatementLease() = default;

    StatementLease(StatementLease&& o) noexcept { move_from(o); }
    StatementLease& operator=(StatementLease&& o) noexcept {
        if (this != &o) {
            release();
            move_from(o);
        }
        return *this;
    }
    StatementLease(const StatementLease&) = delete;
    StatementLease& operator=(const StatementLease&) = delete;
    ~StatementLease() { release(); }

    cli::StatementHandle handle() const noexcept { return handle_; }
    void poison() noexcept { poisoned_ = true; }

  private:
    friend class StatementCache;

    // Transient lease factory: owns the handle and finalizes it on release.
    static StatementLease make_transient(cli::ICliDriver& driver,
                                         cli::StatementHandle h) {
        StatementLease l;
        l.driver_ = &driver;
        l.handle_ = h;
        l.transient_ = true;
        return l;
    }

    void move_from(StatementLease& o) noexcept {
        cache_ = o.cache_;
        entry_ = o.entry_;
        driver_ = o.driver_;
        handle_ = o.handle_;
        transient_ = o.transient_;
        poisoned_ = o.poisoned_;
        o.cache_ = nullptr;
        o.entry_ = nullptr;
        o.driver_ = nullptr;
        o.handle_ = cli::StatementHandle::invalid;
        o.transient_ = false;
        o.poisoned_ = false;
    }

    void release();  // defined after StatementCache (needs its full definition)

    StatementCache* cache_ = nullptr;
    StmtCacheEntry* entry_ = nullptr;
    cli::ICliDriver* driver_ = nullptr;
    cli::StatementHandle handle_ = cli::StatementHandle::invalid;
    bool transient_ = false;
    bool poisoned_ = false;
};

// Per-connection fixed-capacity LRU of prepared statements. NOT thread-safe and
// NOT movable (leases hold raw pointers into it); Connection owns it via a
// unique_ptr so its address is stable across Connection moves.
class StatementCache {
  public:
    StatementCache(cli::ICliDriver& driver, cli::ConnectionHandle conn,
                   std::size_t capacity, obs::MetricsSink* metrics = nullptr)
        : driver_(&driver), conn_(conn), capacity_(capacity), metrics_(metrics) {}

    StatementCache(const StatementCache&) = delete;
    StatementCache& operator=(const StatementCache&) = delete;
    StatementCache(StatementCache&&) = delete;
    StatementCache& operator=(StatementCache&&) = delete;

    ~StatementCache() {
        for (auto& e : lru_) driver_->finalize(e.handle);
    }

    Result<StatementLease> acquire(const std::string& sql) {
        if (capacity_ == 0) {  // disabled: always transient
            emit("overflow");
            return transient(sql);
        }

        auto it = index_.find(sql);
        if (it != index_.end()) {
            StmtCacheEntry* e = &*it->second;
            if (e->busy) {  // overlapping use of the same sql -> transient
                emit("overflow");
                return transient(sql);
            }
            lru_.splice(lru_.begin(), lru_, it->second);  // to MRU
            e->busy = true;
            emit("hit");
            return make_cached(e);
        }

        auto h = driver_->prepare(conn_, sql);
        if (!h.ok()) return h.error();

        if (lru_.size() >= capacity_) {
            StmtCacheEntry* victim = lru_idle_victim();
            if (victim == nullptr) {  // every entry busy -> don't insert
                emit("overflow");
                return StatementLease::make_transient(*driver_, h.value());
            }
            evict(victim);
        }

        lru_.push_front(StmtCacheEntry{sql, h.value(), /*busy=*/true});
        index_[sql] = lru_.begin();
        emit("miss");
        emit_size();
        return make_cached(&lru_.front());
    }

  private:
    friend class StatementLease;

    StatementLease make_cached(StmtCacheEntry* e) {
        StatementLease l;
        l.cache_ = this;
        l.entry_ = e;
        l.handle_ = e->handle;
        return l;
    }

    Result<StatementLease> transient(const std::string& sql) {
        auto h = driver_->prepare(conn_, sql);
        if (!h.ok()) return h.error();
        return StatementLease::make_transient(*driver_, h.value());
    }

    StmtCacheEntry* lru_idle_victim() {
        for (auto rit = lru_.rbegin(); rit != lru_.rend(); ++rit)
            if (!rit->busy) return &*rit;
        return nullptr;
    }

    void erase_entry(StmtCacheEntry* e) {
        index_.erase(e->key);
        for (auto lit = lru_.begin(); lit != lru_.end(); ++lit)
            if (&*lit == e) {
                lru_.erase(lit);
                return;
            }
    }

    void evict(StmtCacheEntry* e) {
        driver_->finalize(e->handle);
        erase_entry(e);
        emit("evict");
        emit_size();
    }

    // Called from StatementLease::release().
    void release_entry(StmtCacheEntry* e, bool poisoned) {
        if (poisoned) {
            driver_->finalize(e->handle);
            erase_entry(e);
            emit_size();
            return;
        }
        driver_->closeCursor(e->handle);  // best-effort reset for reuse
        e->busy = false;
        for (auto lit = lru_.begin(); lit != lru_.end(); ++lit)
            if (&*lit == e) {
                lru_.splice(lru_.begin(), lru_, lit);  // to MRU
                return;
            }
    }

    void emit(const char* result) {
        if (metrics_)
            metrics_->counter("halcyon_stmt_cache_total", 1.0,
                              {{"result", result}});
    }
    void emit_size() {
        if (metrics_)
            metrics_->gauge("halcyon_stmt_cache_size",
                            static_cast<double>(lru_.size()), {});
    }

    cli::ICliDriver* driver_;
    cli::ConnectionHandle conn_;
    std::size_t capacity_;
    obs::MetricsSink* metrics_;
    std::list<StmtCacheEntry> lru_;  // front = most-recently-used
    std::unordered_map<std::string, std::list<StmtCacheEntry>::iterator> index_;
};

// --- StatementLease::release (needs the full StatementCache definition) ---

inline void StatementLease::release() {
    if (transient_) {
        if (driver_ && handle_ != cli::StatementHandle::invalid)
            driver_->finalize(handle_);
    } else if (cache_ && entry_) {
        cache_->release_entry(entry_, poisoned_);
    }
    cache_ = nullptr;
    entry_ = nullptr;
    driver_ = nullptr;
    handle_ = cli::StatementHandle::invalid;
    transient_ = false;
    poisoned_ = false;
}

}  // namespace halcyon::detail
```

The two factories are `StatementLease::make_transient` (private static, used for
the disabled/overflow paths) and `StatementCache::make_cached` (private member,
used for hit/miss). `StatementCache` is `friend` of `StatementLease`, so it can
call `make_transient` and construct cached leases.

- [x] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --target halcyon_unit_tests -j && ctest --test-dir build -R StatementCache --output-on-failure`
Expected: PASS (all 7 `StatementCache.*` tests).

- [x] **Step 6: Commit**

```bash
git add include/halcyon/detail/statement_cache.hpp \
        tests/unit/test_statement_cache.cpp tests/CMakeLists.txt
git commit -m "feat: add per-connection StatementCache with LRU + RAII lease"
```

---

## Task 3: Route `Connection` through the cache

**Files:**
- Modify: `include/halcyon/connection.hpp`
- Modify: `tests/unit/test_connection.cpp`

- [x] **Step 1: Write the failing connection tests**

Append to `tests/unit/test_connection.cpp`:

```cpp
TEST(ConnectionCache, RepeatedQueryReusesPreparedStatement) {
    MockCliDriver driver;
    // Two SELECTs of the same SQL; scripted empty result sets.
    driver.resultSets.push_back({{"id"}, {}});
    driver.resultSets.push_back({{"id"}, {}});
    auto conn = Connection::open(driver, ConnectionParams{"x"},
                                 /*statementCacheSize=*/8)
                    .value();

    { auto r = conn.query("SELECT id FROM t WHERE age > ?", 21); ASSERT_TRUE(r.ok()); }
    { auto r = conn.query("SELECT id FROM t WHERE age > ?", 21); ASSERT_TRUE(r.ok()); }

    EXPECT_EQ(driver.preparedSql.size(), 1u);  // prepared once, reused
}

TEST(ConnectionCache, DisabledByDefaultPreparesEveryCall) {
    MockCliDriver driver;
    driver.resultSets.push_back({{"id"}, {}});
    driver.resultSets.push_back({{"id"}, {}});
    auto conn = Connection::open(driver, ConnectionParams{"x"}).value();  // size 0

    { auto r = conn.query("SELECT id FROM t WHERE age > ?", 21); ASSERT_TRUE(r.ok()); }
    { auto r = conn.query("SELECT id FROM t WHERE age > ?", 21); ASSERT_TRUE(r.ok()); }

    EXPECT_EQ(driver.preparedSql.size(), 2u);  // no cache -> two prepares
}

TEST(ConnectionCache, OverlappingCursorsOnSameSqlOverflow) {
    MockCliDriver driver;
    driver.resultSets.push_back({{"id"}, {}});
    driver.resultSets.push_back({{"id"}, {}});
    auto conn = Connection::open(driver, ConnectionParams{"x"},
                                 /*statementCacheSize=*/8)
                    .value();

    auto r1 = conn.query("SELECT id FROM t", 0);  // held open (busy)
    ASSERT_TRUE(r1.ok());
    auto r2 = conn.query("SELECT id FROM t", 0);  // same sql, still busy
    ASSERT_TRUE(r2.ok());

    EXPECT_EQ(driver.preparedSql.size(), 2u);  // second is a transient overflow
}

TEST(ConnectionCache, ExecuteErrorDropsEntry) {
    MockCliDriver driver;
    driver.executeErrors.push_back([] {
        halcyon::Error e; e.code = halcyon::ErrorCode::Syntax; e.message = "boom";
        return e;
    }());
    auto conn = Connection::open(driver, ConnectionParams{"x"},
                                 /*statementCacheSize=*/8)
                    .value();

    auto bad = conn.execute("UPDATE t SET a = ? WHERE b = ?", 1, 2);
    ASSERT_FALSE(bad.ok());                // first execute fails -> poisoned
    auto ok = conn.execute("UPDATE t SET a = ? WHERE b = ?", 1, 2);
    ASSERT_TRUE(ok.ok());                  // re-prepared after drop

    EXPECT_EQ(driver.preparedSql.size(), 2u);
}
```

- [x] **Step 2: Run to verify failure**

Run: `cmake --build build --target halcyon_unit_tests -j`
Expected: FAIL — `Connection::open` has no 3-arg form; `statementCacheSize` unknown.

- [x] **Step 3: Add the include and cache member**

In `include/halcyon/connection.hpp`, add the includes near the existing ones
(after line 12, the `driver.hpp` include):

```cpp
#include <memory>

#include "halcyon/detail/statement_cache.hpp"
#include "halcyon/observability/metrics.hpp"
```

- [x] **Step 4: Replace `ResultSet::owned_` with a lease**

In the `ResultSet` private section (lines 196-207), replace:

```cpp
    std::optional<Statement> owned_;  // present when the ResultSet owns its statement
```

with:

```cpp
    std::optional<detail::StatementLease> lease_;  // owns the cached/transient stmt
```

(The borrowing `create_borrowing` path and `Statement::execute_query` leave
`lease_` empty, exactly as they left `owned_` empty.)

- [x] **Step 5: Update `Connection` ctor/open and add the cache member**

Replace the `Connection::open` (lines 222-227) and the constructor (lines 229-230)
with:

```cpp
    static Result<Connection> open(detail::cli::ICliDriver& driver,
                                   const detail::cli::ConnectionParams& params,
                                   std::size_t statementCacheSize = 0,
                                   obs::MetricsSink* metrics = nullptr) {
        auto h = driver.connect(params);
        if (!h.ok()) return h.error();
        return Connection(driver, h.value(), statementCacheSize, metrics);
    }

    Connection(detail::cli::ICliDriver& driver,
               detail::cli::ConnectionHandle handle,
               std::size_t statementCacheSize = 0,
               obs::MetricsSink* metrics = nullptr)
        : driver_(&driver),
          handle_(handle),
          cache_(std::make_unique<detail::StatementCache>(
              driver, handle, statementCacheSize, metrics)) {}
```

Update the move-ctor and move-assignment (lines 232-243) to also move `cache_`:

```cpp
    Connection(Connection&& o) noexcept
        : driver_(o.driver_), handle_(o.handle_), cache_(std::move(o.cache_)) {
        o.handle_ = detail::cli::ConnectionHandle::invalid;
    }
    Connection& operator=(Connection&& o) noexcept {
        if (this != &o) {
            reset();
            driver_ = o.driver_;
            handle_ = o.handle_;
            cache_ = std::move(o.cache_);
            o.handle_ = detail::cli::ConnectionHandle::invalid;
        }
        return *this;
    }
```

Add the member next to `handle_` (after line 377):

```cpp
    std::unique_ptr<detail::StatementCache> cache_;
```

- [x] **Step 6: Route the public methods through the cache**

Replace the anonymous-arg `query` (lines 259-265), `execute` (267-273), the named
overloads (276-292), `queryAs` (295-310), `executeBatch` (314-329), and the
private helpers `collect` (335-354) and `run_query` (357-368) with the
lease-based versions below. Keep `prepare` (252-256) unchanged (explicit prepare
stays non-caching).

```cpp
    // --- anonymous-parameter overloads ---
    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<ResultSet> query(const std::string& sql, const Args&... args) {
        auto lease = cache_->acquire(sql);
        if (!lease.ok()) return lease.error();
        return run_query(std::move(lease.value()), detail::pack_params(args...));
    }

    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::int64_t> execute(const std::string& sql, const Args&... args) {
        auto lease = cache_->acquire(sql);
        if (!lease.ok()) return lease.error();
        return exec_lease(lease.value(), detail::pack_params(args...));
    }

    // --- named-parameter overloads ---
    Result<ResultSet> query(const std::string& sql, const params& named) {
        auto pre = detail::bind_named(sql, named);
        if (!pre.ok()) return pre.error();
        auto lease = cache_->acquire(pre.value().sql);
        if (!lease.ok()) return lease.error();
        return run_query(std::move(lease.value()), pre.value().params);
    }

    Result<std::int64_t> execute(const std::string& sql, const params& named) {
        auto pre = detail::bind_named(sql, named);
        if (!pre.ok()) return pre.error();
        auto lease = cache_->acquire(pre.value().sql);
        if (!lease.ok()) return lease.error();
        return exec_lease(lease.value(), pre.value().params);
    }

    // --- struct-mapping overloads (require HALCYON_REFLECT(T, ...)) ---
    template <class T, class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::vector<T>> queryAs(const std::string& sql, const Args&... args) {
        auto lease = cache_->acquire(sql);
        if (!lease.ok()) return lease.error();
        return collect<T>(lease.value(), detail::pack_params(args...));
    }

    template <class T>
    Result<std::vector<T>> queryAs(const std::string& sql, const params& named) {
        auto pre = detail::bind_named(sql, named);
        if (!pre.ok()) return pre.error();
        auto lease = cache_->acquire(pre.value().sql);
        if (!lease.ok()) return lease.error();
        return collect<T>(lease.value(), pre.value().params);
    }

    // Prepares sql once (cached) and executes it for each row of positional
    // params, accumulating affected-row counts. Stops at the first error.
    Result<std::int64_t> executeBatch(
        const std::string& sql,
        const std::vector<std::vector<detail::cli::Value>>& rows) {
        if (rows.empty()) return std::int64_t{0};
        auto lease = cache_->acquire(sql);
        if (!lease.ok()) return lease.error();
        std::int64_t total = 0;
        for (const auto& row : rows) {
            auto b = driver_->bindParams(lease.value().handle(), row);
            if (!b.ok()) {
                lease.value().poison();
                return b.error();
            }
            auto e = driver_->execute(lease.value().handle());
            if (!e.ok()) {
                lease.value().poison();
                return e.error();
            }
            total += e.value();
        }
        return total;
    }

    // Begins a transaction (autocommit OFF). Defined in transaction.hpp.
    Result<Transaction> begin();

  private:
    Result<std::int64_t> exec_lease(
        detail::StatementLease& lease,
        const std::vector<detail::cli::Value>& params) {
        auto b = driver_->bindParams(lease.handle(), params);
        if (!b.ok()) {
            lease.poison();
            return b.error();
        }
        auto e = driver_->execute(lease.handle());
        if (!e.ok()) {
            lease.poison();
            return e.error();
        }
        return e.value();
    }

    template <class T>
    Result<std::vector<T>> collect(
        detail::StatementLease& lease,
        const std::vector<detail::cli::Value>& params) {
        auto b = driver_->bindParams(lease.handle(), params);
        if (!b.ok()) {
            lease.poison();
            return b.error();
        }
        auto e = driver_->execute(lease.handle());
        if (!e.ok()) {
            lease.poison();
            return e.error();
        }
        auto cc = driver_->columnCount(lease.handle());
        if (!cc.ok()) {
            lease.poison();
            return cc.error();
        }
        std::vector<T> out;
        for (;;) {
            auto f = driver_->fetch(lease.handle());
            if (!f.ok()) {
                lease.poison();
                return f.error();
            }
            if (!f.value()) break;
            auto row = reflect::map_row<T>(*driver_, lease.handle(), cc.value());
            if (!row.ok()) {
                lease.poison();
                return row.error();
            }
            out.push_back(std::move(row.value()));
        }
        return out;
    }

    // Builds a ResultSet that OWNS the lease (kept alive for the cursor's life;
    // the lease closes the cursor and returns the statement to the cache on
    // destruction).
    Result<ResultSet> run_query(detail::StatementLease&& lease,
                                const std::vector<detail::cli::Value>& params) {
        auto b = driver_->bindParams(lease.handle(), params);
        if (!b.ok()) {
            lease.poison();
            return b.error();
        }
        auto e = driver_->execute(lease.handle());
        if (!e.ok()) {
            lease.poison();
            return e.error();
        }
        auto cc = driver_->columnCount(lease.handle());
        if (!cc.ok()) {
            lease.poison();
            return cc.error();
        }
        ResultSet rs(driver_, lease.handle(), cc.value());
        rs.lease_ = std::move(lease);
        return rs;
    }
```

Note: the old code had a single `private:` block; after this edit there is one
`private:` opening the helpers above, followed by the existing `reset()` and the
data members. Ensure there is exactly one `private:` label introducing
`exec_lease`/`collect`/`run_query`/`reset` and the members — merge with the
existing private section rather than duplicating the label.

- [x] **Step 7: Run the connection tests to verify they pass**

Run: `cmake --build build --target halcyon_unit_tests -j && ctest --test-dir build -R "Connection" --output-on-failure`
Expected: PASS — new `ConnectionCache.*` tests pass and all pre-existing
`Connection*`/`ResultSet*` tests still pass (single-call tests are unaffected;
the default capacity is 0).

- [x] **Step 8: Run the full unit suite (no regressions)**

Run: `ctest --test-dir build --output-on-failure`
Expected: PASS — entire unit suite green.

- [x] **Step 9: Commit**

```bash
git add include/halcyon/connection.hpp tests/unit/test_connection.cpp
git commit -m "feat: route Connection query/execute paths through StatementCache"
```

---

## Task 4: Wire `PoolConfig.statementCacheSize` into the pool

**Files:**
- Modify: `include/halcyon/pool.hpp`
- Modify: `tests/unit/test_pool.cpp`

- [x] **Step 1: Write the failing pool tests**

Append to `tests/unit/test_pool.cpp` (uses the existing `MockCliDriver`/
`PoolConfig` includes already present in that file):

```cpp
TEST(PoolStatementCache, ReusesPreparedStatementAcrossAcquires) {
    halcyon::testing::MockCliDriver driver;
    driver.resultSets.push_back({{"id"}, {}});
    driver.resultSets.push_back({{"id"}, {}});
    halcyon::PoolConfig cfg;
    cfg.min = 1;
    cfg.max = 1;                       // single physical connection, reused
    cfg.startMaintenanceThread = false;
    cfg.statementCacheSize = 8;
    auto pool =
        halcyon::ConnectionPool::create(driver, {"dsn"}, cfg).value();

    {
        auto lease = pool->acquire().value();
        auto r = lease->query("SELECT id FROM t", 1);
        ASSERT_TRUE(r.ok());
    }  // returned to pool; cache warm
    {
        auto lease = pool->acquire().value();
        auto r = lease->query("SELECT id FROM t", 1);
        ASSERT_TRUE(r.ok());
    }

    EXPECT_EQ(driver.preparedSql.size(), 1u);  // same connection -> reuse
}

TEST(PoolStatementCache, ReconnectStartsWithFreshCache) {
    halcyon::testing::MockCliDriver driver;
    driver.resultSets.push_back({{"id"}, {}});
    driver.resultSets.push_back({{"id"}, {}});
    halcyon::PoolConfig cfg;
    cfg.min = 1;
    cfg.max = 1;
    cfg.startMaintenanceThread = false;
    cfg.validateOnAcquire = true;       // validate (isAlive) on each acquire
    cfg.statementCacheSize = 8;
    auto pool =
        halcyon::ConnectionPool::create(driver, {"dsn"}, cfg).value();

    {
        auto lease = pool->acquire().value();
        auto r = lease->query("SELECT id FROM t", 1);  // prepare #1
        ASSERT_TRUE(r.ok());
    }
    driver.aliveResults.push_back(false);  // next acquire sees a dead connection
    {
        auto lease = pool->acquire().value();             // reconnect -> new Connection
        auto r = lease->query("SELECT id FROM t", 1);     // prepare #2 (fresh cache)
        ASSERT_TRUE(r.ok());
    }

    EXPECT_EQ(driver.preparedSql.size(), 2u);  // cache did not survive reconnect
}
```

- [x] **Step 2: Run to verify failure**

Run: `cmake --build build --target halcyon_unit_tests -j`
Expected: FAIL — `PoolConfig` has no member `statementCacheSize`.

- [x] **Step 3: Add the config field**

In `include/halcyon/pool.hpp`, add to `PoolConfig` (after line 36,
`obs::ObservabilityConfig observability{};`):

```cpp
    std::size_t statementCacheSize = 64;  // per-connection prepared-stmt LRU; 0 disables
```

- [x] **Step 4: Inject it when creating connections**

In `ConnectionPool::make_connection()` (lines 289-305), change the
`Connection::open` call (line 297) from:

```cpp
            auto c = Connection::open(*driver_, params_);
```

to:

```cpp
            auto c = Connection::open(*driver_, params_,
                                      config_.statementCacheSize,
                                      has_metrics_ ? metrics_ : nullptr);
```

(The `metrics_`/`has_metrics_` members are resolved in the pool ctor, lines
235-240, so they are valid here.)

- [x] **Step 5: Run the pool tests to verify they pass**

Run: `cmake --build build --target halcyon_unit_tests -j && ctest --test-dir build -R "PoolStatementCache" --output-on-failure`
Expected: PASS.

- [x] **Step 6: Run the full unit suite (no regressions)**

Run: `ctest --test-dir build --output-on-failure`
Expected: PASS — including the existing `test_database_batch` "single prepare
reused" test, which now also benefits from the pool default cache.

- [x] **Step 7: Commit**

```bash
git add include/halcyon/pool.hpp tests/unit/test_pool.cpp
git commit -m "feat: PoolConfig.statementCacheSize injects per-connection cache (default 64)"
```

---

## Task 5: Integration coverage against live Db2 (gated)

**Files:**
- Modify: `tests/integration/test_db2_roundtrip.cpp`

These run only when `HALCYON_TEST_DSN` is set (see AGENTS.md). They validate the
real `closeCursor`-based reuse path that the mock cannot.

- [x] **Step 1: Add the reuse + cursor-correctness integration tests**

Append to `tests/integration/test_db2_roundtrip.cpp` (follow the file's existing
DSN-gating pattern — read the top of the file and reuse its skip macro / fixture;
the snippet below assumes a `Database` opened from `HALCYON_TEST_DSN` exactly as
the existing tests do):

```cpp
TEST(Db2Integration, CachedStatementReuseReturnsCorrectRows) {
    const char* dsn = std::getenv("HALCYON_TEST_DSN");
    if (dsn == nullptr) GTEST_SKIP() << "HALCYON_TEST_DSN not set";

    halcyon::PoolConfig cfg;
    cfg.min = 1;
    cfg.max = 1;                      // force a single reused physical connection
    cfg.statementCacheSize = 8;
    auto db = halcyon::Database::open(dsn, cfg).value();

    // Same SELECT executed twice with different params on the cached statement:
    // the second execute must close the first cursor (closeCursor) and return the
    // correct rows, proving the cached handle is reusable.
    auto one = db.queryAsOrThrow<int>(
        "SELECT CAST(? AS INTEGER) FROM SYSIBM.SYSDUMMY1", 1);
    auto two = db.queryAsOrThrow<int>(
        "SELECT CAST(? AS INTEGER) FROM SYSIBM.SYSDUMMY1", 2);

    ASSERT_EQ(one.size(), 1u);
    ASSERT_EQ(two.size(), 1u);
    EXPECT_EQ(one[0], 1);
    EXPECT_EQ(two[0], 2);
}
```

If `queryAsOrThrow<int>` mapping a single scalar column is not already supported
by the reflection/tuple layer in this codebase, use the tuple form instead:
`db.queryOrThrow(...)` then `row.as<int>()` inside the range-for, matching how the
other integration tests in this file read scalar columns. (Read the existing tests
first and mirror their exact row-reading style — do not invent a new one.)

- [x] **Step 2: Build with integration enabled**

Run:
```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON -DHALCYON_BUILD_INTEGRATION_TESTS=ON
cmake --build build -j
```
Expected: builds clean.

- [x] **Step 3: Bring up Db2 and run the integration suite**

Run (per AGENTS.md):
```bash
docker compose -f docker/docker-compose.yml up -d
docker compose -f docker/docker-compose.yml ps   # wait for STATUS = healthy
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
ctest --test-dir build -L integration --output-on-failure
```
Expected: PASS — `Db2Integration.CachedStatementReuseReturnsCorrectRows` runs
(not skipped) and passes, confirming real cursor reuse.

- [x] **Step 4: Tear down**

Run: `docker compose -f docker/docker-compose.yml down`

- [x] **Step 5: Commit**

```bash
git add tests/integration/test_db2_roundtrip.cpp
git commit -m "test: integration coverage for cached prepared-statement reuse"
```

---

## Final verification (per AGENTS.md — run the WHOLE suite)

- [x] **Unit suite:**

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```
Expected: all unit tests PASS.

- [x] **Integration suite (live Db2 — NOT optional; a Skipped result is not verification):**

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON -DHALCYON_BUILD_INTEGRATION_TESTS=ON
cmake --build build -j
docker compose -f docker/docker-compose.yml up -d
docker compose -f docker/docker-compose.yml ps   # wait for healthy
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
ctest --test-dir build -L integration --output-on-failure
docker compose -f docker/docker-compose.yml down
```
Expected: integration tests RUN (not skipped) and PASS.

- [x] **Style:** run `clang-format` and `clang-tidy` on the touched files; ensure
  `-Wall -Wextra -Wpedantic` clean (build with `-DHALCYON_WARNINGS_AS_ERRORS=ON`).

---

## Self-review notes (addressed in this plan)

- **Spec coverage:** seam `closeCursor` (Task 1) ✓; cache key = verbatim prepared
  SQL — `query(named)` acquires on `bind_named(...).sql`, anonymous on raw `sql`
  (Task 3) ✓; transparent activation + capacity via `PoolConfig` default 64,
  direct `Connection` default 0 (Tasks 3-4) ✓; busy→overflow (Tasks 2-3) ✓;
  LRU eviction skips busy entries, finalizes victim (Task 2) ✓; poison-on-error
  drop (Tasks 2-3) ✓; reconnect = fresh connection/cache, structural (Task 4) ✓;
  destructor finalizes (Task 2) ✓; metrics counter+gauge (Task 2) ✓;
  thread-safety = none internal, single-owner (design contract) ✓.
- **Type consistency:** `StmtCacheEntry`, `StatementLease` (`handle()`,
  `poison()`, `make_transient`), `StatementCache` (`acquire`, `release_entry`,
  `evict`, `make_cached`), `ResultSet::lease_`, `Connection` ctor/`open`
  `(driver, params|handle, statementCacheSize, metrics)`, and
  `PoolConfig.statementCacheSize` are used identically across Tasks 2-4.
```
