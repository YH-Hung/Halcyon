# Halcyon Pool & Concurrency Implementation Plan (Plan 3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the concurrency layer on top of the Plan 2 core: a portable, mockable
backoff/retry policy (`BackoffPolicy`, `ExecPolicy`, `with_retry`, read-only
classification), a thread-safe `ConnectionPool` (`PoolConfig`, lazy growth `min`→
`max`, `acquire()` with timeout, `PooledConnection` RAII), validation +
transparent reconnect with bounded exponential backoff, a background maintenance
reaper (idle/lifetime expiry + refill to `min`), and a `std::future` async
`Executor` with a single `submit` chokepoint that a future C++20 coroutine layer
can wrap without API breakage.

**Architecture:** Everything in this plan lives in the **pool** layer, strictly
above the seam (`detail::cli::ICliDriver`) and above the **core**
(`Connection`/`Statement`/`ResultSet`). It depends only on `Connection` (Plan 2)
and the portable error/result types (Plan 1). It never includes `sqlcli1.h`, so
the **invariant** from `AGENTS.md` holds and the entire layer is unit-tested
against `MockCliDriver` with **no live Db2**. Time is injected via a `Clock`
functor in `PoolConfig` (so the reaper is deterministic in tests) and sleeps are
injected via `BackoffPolicy::sleep` (so backoff tests run instantly).

**Tech Stack:** C++17, CMake ≥ 3.20, GoogleTest (FetchContent, from Plan 1),
`<thread>`/`<mutex>`/`<condition_variable>`/`<future>` from the standard library.
No new third-party dependencies. The vendored IBM Db2 CLI driver is only touched
by the (gated) integration test.

**Builds on (Plans 1–2, merged):** `include/halcyon/{version,error,result,types,
parameters,connection}.hpp`, `include/halcyon/detail/cli/{driver,sqlstate,
db2_cli_driver}.hpp`, `tests/unit/mock_cli_driver.hpp` (scriptable connect /
isAlive / statement engine), the CMake target `halcyon::halcyon`, and the
GoogleTest harness.

---

## Design contracts (shared across tasks — keep consistent)

These names/signatures are referenced by multiple tasks. Treat them as the source
of truth; if a later task needs a change, update this section in the same commit.

**Backoff + retry (Task 1, `retry.hpp`):**

```cpp
struct BackoffPolicy {
    std::chrono::milliseconds baseDelay{50};
    std::chrono::milliseconds maxDelay{2000};
    int maxAttempts = 3;  // used by reconnect loop (Task 3)
    std::function<void(std::chrono::milliseconds)> sleep =
        [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); };
    std::chrono::milliseconds delay_for(int attempt) const;  // capped exp backoff
};

struct ExecPolicy {
    int maxAttempts = 1;       // 1 == no retry
    BackoffPolicy backoff{};
    static ExecPolicy once();              // maxAttempts == 1
    static ExecPolicy idempotent(int attempts = 3);
};

// Retries fn() (which returns Result<T>) while the error is retriable and
// attempts remain, sleeping per policy.backoff between tries.
template <class Fn> auto with_retry(const ExecPolicy& policy, Fn&& fn);

// True for read-only statements safe to auto-retry (SELECT / WITH…SELECT / VALUES).
bool detail::is_read_only(std::string_view sql) noexcept;
```

**Pool (Tasks 2–4, `pool.hpp`):**

```cpp
using Clock = std::chrono::steady_clock;

struct PoolConfig {
    std::size_t min = 1;
    std::size_t max = 8;
    std::chrono::milliseconds acquireTimeout{5000};
    std::chrono::milliseconds idleTimeout{60000};
    std::chrono::milliseconds maxLifetime{1800000};
    bool validateOnAcquire = false;
    BackoffPolicy backoff{};                       // reconnect backoff
    std::function<Clock::time_point()> now = [] { return Clock::now(); };
    std::chrono::milliseconds maintenanceInterval{1000};
    bool startMaintenanceThread = true;            // tests disable + call maintain()
};

namespace detail { struct PoolSlot { /* conn + timestamps */ }; }

class ConnectionPool;   // non-copyable, non-movable; created via ::create(...)
class PooledConnection; // RAII; operator-> exposes Connection; markBroken()
```

**Async (Task 5, `async.hpp`):**

```cpp
class Executor {                                   // fixed-size worker pool
    explicit Executor(std::size_t threads);
    template <class Fn> auto submit(Fn&& fn)        // single chokepoint
        -> std::future<std::invoke_result_t<std::decay_t<Fn>>>;
};

// Acquires a pooled connection on a worker thread and runs fn(Connection&).
template <class Fn>
auto async_with_connection(Executor& ex, ConnectionPool& pool, Fn fn);
```

---

## File Structure (created/modified by this plan)

- `include/halcyon/retry.hpp` — **create**: `BackoffPolicy`, `ExecPolicy`, `with_retry`, `is_read_only`.
- `include/halcyon/pool.hpp` — **create**: `PoolConfig`, `detail::PoolSlot`, `ConnectionPool`, `PooledConnection`.
- `include/halcyon/async.hpp` — **create**: `Executor`, `async_with_connection`.
- `include/halcyon/halcyon.hpp` — **modify**: include the three new headers.
- `tests/unit/test_retry.cpp` — **create**.
- `tests/unit/test_pool.cpp` — **create** (basics + validation + reconnect + broken).
- `tests/unit/test_pool_maintenance.cpp` — **create** (reaper + refill, fake clock).
- `tests/unit/test_async.cpp` — **create**.
- `tests/CMakeLists.txt` — **modify**: register the new unit tests; link `Threads::Threads`.
- `tests/integration/test_db2_roundtrip.cpp` — **modify**: add a gated pool concurrency test.

---

## Task 1: Backoff + retry policy (`retry.hpp`)

This is pure, portable C++ and must be unit-tested without a DB or real sleeping
(sleep is injected). Tasks 3 and 5 reuse it.

**Files:**
- Create: `include/halcyon/retry.hpp`
- Modify: `tests/CMakeLists.txt` (add `unit/test_retry.cpp`)
- Test: `tests/unit/test_retry.cpp`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_retry.cpp`:

```cpp
#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "halcyon/result.hpp"
#include "halcyon/retry.hpp"

using halcyon::BackoffPolicy;
using halcyon::ErrorCode;
using halcyon::ExecPolicy;
using halcyon::Result;
using halcyon::with_retry;
using namespace std::chrono_literals;

namespace {
halcyon::Error err(ErrorCode code, bool retriable) {
    halcyon::Error e;
    e.code = code;
    e.retriable = retriable;
    e.message = "scripted";
    return e;
}
}  // namespace

TEST(Backoff, ExponentialAndCapped) {
    BackoffPolicy b;
    b.baseDelay = 10ms;
    b.maxDelay = 50ms;
    EXPECT_EQ(b.delay_for(1), 10ms);
    EXPECT_EQ(b.delay_for(2), 20ms);
    EXPECT_EQ(b.delay_for(3), 40ms);
    EXPECT_EQ(b.delay_for(4), 50ms);  // capped
    EXPECT_EQ(b.delay_for(99), 50ms);  // no overflow, still capped
}

TEST(WithRetry, ReturnsImmediatelyOnSuccess) {
    int calls = 0;
    auto policy = ExecPolicy::idempotent(3);
    policy.backoff.sleep = [](std::chrono::milliseconds) {};
    auto r = with_retry(policy, [&]() -> Result<int> {
        ++calls;
        return 42;
    });
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 42);
    EXPECT_EQ(calls, 1);
}

TEST(WithRetry, RetriesRetriableUpToMaxAttempts) {
    int calls = 0;
    std::vector<std::chrono::milliseconds> slept;
    auto policy = ExecPolicy::idempotent(3);
    policy.backoff.baseDelay = 5ms;
    policy.backoff.sleep = [&](std::chrono::milliseconds d) { slept.push_back(d); };
    auto r = with_retry(policy, [&]() -> Result<int> {
        ++calls;
        return err(ErrorCode::Transient, true);
    });
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Transient);
    EXPECT_EQ(calls, 3);            // initial + 2 retries
    EXPECT_EQ(slept.size(), 2u);   // slept before each retry
}

TEST(WithRetry, DoesNotRetryNonRetriable) {
    int calls = 0;
    auto policy = ExecPolicy::idempotent(5);
    policy.backoff.sleep = [](std::chrono::milliseconds) {};
    auto r = with_retry(policy, [&]() -> Result<int> {
        ++calls;
        return err(ErrorCode::Constraint, false);
    });
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(calls, 1);
}

TEST(WithRetry, OncePolicyNeverRetries) {
    int calls = 0;
    auto policy = ExecPolicy::once();
    policy.backoff.sleep = [](std::chrono::milliseconds) {};
    auto r = with_retry(policy, [&]() -> Result<int> {
        ++calls;
        return err(ErrorCode::Transient, true);
    });
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(calls, 1);
}

TEST(IsReadOnly, RecognizesSafeStatements) {
    using halcyon::detail::is_read_only;
    EXPECT_TRUE(is_read_only("SELECT 1 FROM SYSIBM.SYSDUMMY1"));
    EXPECT_TRUE(is_read_only("  \n select id from t"));
    EXPECT_TRUE(is_read_only("WITH x AS (SELECT 1) SELECT * FROM x"));
    EXPECT_TRUE(is_read_only("VALUES (1)"));
    EXPECT_FALSE(is_read_only("INSERT INTO t VALUES (1)"));
    EXPECT_FALSE(is_read_only("UPDATE t SET a = 1"));
    EXPECT_FALSE(is_read_only("DELETE FROM t"));
    EXPECT_FALSE(is_read_only(""));
}
```

- [ ] **Step 2: Run test to verify it fails**

Add `unit/test_retry.cpp` to `tests/CMakeLists.txt` (Task 6 shows the final list),
reconfigure, build. Expected: compile FAIL — `halcyon/retry.hpp` not found.

- [ ] **Step 3: Write the retry header**

Create `include/halcyon/retry.hpp`:

```cpp
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <string_view>
#include <thread>
#include <utility>

namespace halcyon {

// Exponential backoff with a hard cap and an injectable sleep (so tests run
// instantly and the reconnect loop can be driven deterministically).
struct BackoffPolicy {
    std::chrono::milliseconds baseDelay{50};
    std::chrono::milliseconds maxDelay{2000};
    int maxAttempts = 3;
    std::function<void(std::chrono::milliseconds)> sleep =
        [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); };

    // delay for a 1-based attempt index: baseDelay * 2^(attempt-1), capped.
    std::chrono::milliseconds delay_for(int attempt) const {
        if (attempt < 1) attempt = 1;
        long long mult = 1;
        for (int i = 1; i < attempt && mult < (1LL << 30); ++i) mult <<= 1;
        auto scaled = baseDelay * mult;
        return scaled > maxDelay ? maxDelay : scaled;
    }
};

// Per-call retry policy. Idempotency is expressed by allowing > 1 attempt; the
// caller (or pool/facade) decides which statements are safe to replay.
struct ExecPolicy {
    int maxAttempts = 1;
    BackoffPolicy backoff{};

    static ExecPolicy once() { return ExecPolicy{1, {}}; }
    static ExecPolicy idempotent(int attempts = 3) {
        ExecPolicy p;
        p.maxAttempts = attempts < 1 ? 1 : attempts;
        return p;
    }
};

// Calls fn() (returns Result<T>); while the result is an error that is retriable
// and attempts remain, sleeps per backoff and retries. Returns the last result.
template <class Fn>
auto with_retry(const ExecPolicy& policy, Fn&& fn) {
    auto r = fn();
    for (int attempt = 1; attempt < policy.maxAttempts; ++attempt) {
        if (r.ok() || !r.error().retriable) return r;
        policy.backoff.sleep(policy.backoff.delay_for(attempt));
        r = fn();
    }
    return r;
}

namespace detail {

// True for statements safe to auto-retry: read-only SELECT / WITH…SELECT / VALUES.
inline bool is_read_only(std::string_view sql) noexcept {
    std::size_t i = 0;
    while (i < sql.size() &&
           std::isspace(static_cast<unsigned char>(sql[i]))) {
        ++i;
    }
    auto starts = [&](std::string_view kw) {
        if (sql.size() - i < kw.size()) return false;
        for (std::size_t k = 0; k < kw.size(); ++k) {
            if (std::toupper(static_cast<unsigned char>(sql[i + k])) != kw[k])
                return false;
        }
        return true;
    };
    return starts("SELECT") || starts("WITH") || starts("VALUES");
}

}  // namespace detail

}  // namespace halcyon
```

- [ ] **Step 4: Wire into the build**

Add `unit/test_retry.cpp` to `halcyon_unit_tests` in `tests/CMakeLists.txt`.

- [ ] **Step 5: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R "Backoff|WithRetry|IsReadOnly" --output-on-failure
```

Expected: all retry tests PASS.

- [ ] **Step 6: Commit**

```bash
git add include/halcyon/retry.hpp tests/CMakeLists.txt tests/unit/test_retry.cpp
git commit -m "feat: add backoff/retry policy and read-only classification"
```

---

## Task 2: `ConnectionPool` core — acquire / release / lazy growth / timeout

Introduces `pool.hpp` with `PoolConfig`, `detail::PoolSlot`, `PooledConnection`,
and `ConnectionPool` with eager warm-up to `min`, lazy growth to `max`, blocking
`acquire()` with timeout, and RAII release. Validation/reconnect (Task 3) and the
reaper (Task 4) are appended to the same header.

**Files:**
- Create: `include/halcyon/pool.hpp` (core portion)
- Modify: `tests/CMakeLists.txt` (add `unit/test_pool.cpp`, link `Threads::Threads`)
- Test: `tests/unit/test_pool.cpp` (basics portion)

- [ ] **Step 1: Write the failing test (basics)**

Create `tests/unit/test_pool.cpp`:

```cpp
#include <gtest/gtest.h>

#include <chrono>

#include "halcyon/pool.hpp"
#include "mock_cli_driver.hpp"

using halcyon::ConnectionPool;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;
using namespace std::chrono_literals;

namespace {
PoolConfig noThread(PoolConfig cfg = {}) {
    cfg.startMaintenanceThread = false;
    return cfg;
}
}  // namespace

TEST(PoolBasics, WarmsUpToMinOnCreate) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 2;
    cfg.max = 4;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg);
    ASSERT_TRUE(pool.ok()) << pool.error().message;
    EXPECT_EQ(pool.value()->total_count(), 2u);
    EXPECT_EQ(pool.value()->idle_count(), 2u);
    EXPECT_EQ(driver.connectCalls, 2);
}

TEST(PoolBasics, AcquireAndReleaseRoundTrips) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 2;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    {
        auto pc = pool->acquire();
        ASSERT_TRUE(pc.ok());
        EXPECT_EQ(pool->idle_count(), 0u);
        EXPECT_EQ(pool->active_count(), 1u);
    }
    EXPECT_EQ(pool->idle_count(), 1u);
    EXPECT_EQ(pool->active_count(), 0u);
}

TEST(PoolBasics, LazyGrowthUpToMax) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 3;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    EXPECT_EQ(driver.connectCalls, 1);  // only min warmed

    auto a = pool->acquire();
    auto b = pool->acquire();
    auto c = pool->acquire();
    ASSERT_TRUE(a.ok() && b.ok() && c.ok());
    EXPECT_EQ(pool->total_count(), 3u);
    EXPECT_EQ(driver.connectCalls, 3);  // grew to max
}

TEST(PoolBasics, AcquireTimesOutWhenExhausted) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    cfg.acquireTimeout = 30ms;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();

    auto held = pool->acquire();
    ASSERT_TRUE(held.ok());
    auto blocked = pool->acquire();
    ASSERT_FALSE(blocked.ok());
    EXPECT_EQ(blocked.error().code, halcyon::ErrorCode::Pool);
}

TEST(PoolBasics, ReleasedConnectionIsReused) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 2;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    { auto pc = pool->acquire(); ASSERT_TRUE(pc.ok()); }
    auto pc2 = pool->acquire();
    ASSERT_TRUE(pc2.ok());
    EXPECT_EQ(driver.connectCalls, 1);  // reused, not reconnected
}

TEST(PoolBasics, PooledConnectionRunsQueries) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"}, {{halcyon::detail::cli::Value{std::int64_t{7}}}}});
    auto pool = ConnectionPool::create(driver, {"x"}, noThread()).value();

    auto pc = pool->acquire();
    ASSERT_TRUE(pc.ok());
    auto rs = pc.value()->query("SELECT id FROM t");
    ASSERT_TRUE(rs.ok());
    long sum = 0;
    for (auto& row : rs.value()) sum += std::get<0>(row.as<int>());
    EXPECT_EQ(sum, 7);
}
```

- [ ] **Step 2: Run test to verify it fails**

Add `unit/test_pool.cpp` to `tests/CMakeLists.txt`, reconfigure, build. Expected:
compile FAIL — `halcyon/pool.hpp` not found.

- [ ] **Step 3: Write `pool.hpp` (core)**

Create `include/halcyon/pool.hpp`:

```cpp
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "halcyon/connection.hpp"
#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/error.hpp"
#include "halcyon/result.hpp"
#include "halcyon/retry.hpp"

namespace halcyon {

using Clock = std::chrono::steady_clock;

struct PoolConfig {
    std::size_t min = 1;
    std::size_t max = 8;
    std::chrono::milliseconds acquireTimeout{5000};
    std::chrono::milliseconds idleTimeout{60000};
    std::chrono::milliseconds maxLifetime{1800000};
    bool validateOnAcquire = false;
    BackoffPolicy backoff{};                       // reconnect backoff
    std::function<Clock::time_point()> now = [] { return Clock::now(); };
    std::chrono::milliseconds maintenanceInterval{1000};
    bool startMaintenanceThread = true;
};

namespace detail {

// Internal bookkeeping for one physical connection owned by the pool.
struct PoolSlot {
    std::unique_ptr<Connection> conn;
    Clock::time_point created_at{};
    Clock::time_point last_used_at{};
};

}  // namespace detail

class ConnectionPool;

// RAII lease on a pooled connection. Returns the connection to the pool on
// destruction (or discards it for replacement if marked broken). Move-only.
class PooledConnection {
public:
    PooledConnection(PooledConnection&& o) noexcept
        : pool_(o.pool_), slot_(o.slot_), broken_(o.broken_) {
        o.pool_ = nullptr;
        o.slot_ = nullptr;
    }
    PooledConnection& operator=(PooledConnection&& o) noexcept {
        if (this != &o) {
            release();
            pool_ = o.pool_;
            slot_ = o.slot_;
            broken_ = o.broken_;
            o.pool_ = nullptr;
            o.slot_ = nullptr;
        }
        return *this;
    }
    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;
    ~PooledConnection() { release(); }

    Connection& operator*() const { return *slot_->conn; }
    Connection* operator->() const { return slot_->conn.get(); }
    Connection* get() const { return slot_ ? slot_->conn.get() : nullptr; }

    // Marks the underlying connection as unusable; on return it is discarded and
    // replaced (rather than handed to the next acquirer).
    void markBroken() noexcept { broken_ = true; }

private:
    friend class ConnectionPool;
    PooledConnection(ConnectionPool* pool, detail::PoolSlot* slot)
        : pool_(pool), slot_(slot) {}
    void release();  // defined after ConnectionPool

    ConnectionPool* pool_;
    detail::PoolSlot* slot_;
    bool broken_ = false;
};

// Thread-safe connection pool: lazy growth min→max, blocking acquire with
// timeout, transparent reconnect, and a background reaper. Created via create();
// non-copyable and non-movable (owns a mutex + maintenance thread).
class ConnectionPool {
public:
    static Result<std::unique_ptr<ConnectionPool>> create(
        detail::cli::ICliDriver& driver, detail::cli::ConnectionParams params,
        PoolConfig config) {
        std::unique_ptr<ConnectionPool> pool(
            new ConnectionPool(driver, std::move(params), std::move(config)));
        for (std::size_t i = 0; i < pool->config_.min; ++i) {
            auto c = pool->make_connection();
            if (!c.ok()) return c.error();
            pool->add_idle_slot(std::move(c.value()));
        }
        if (pool->config_.startMaintenanceThread) pool->start_maintenance();
        return pool;
    }

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;
    ConnectionPool(ConnectionPool&&) = delete;
    ConnectionPool& operator=(ConnectionPool&&) = delete;

    ~ConnectionPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stopping_ = true;
        }
        maint_cv_.notify_all();
        cv_.notify_all();
        if (maintenance_.joinable()) maintenance_.join();
        std::lock_guard<std::mutex> lk(mu_);
        idle_.clear();
        slots_.clear();  // Connection dtors disconnect
    }

    Result<PooledConnection> acquire() {
        std::unique_lock<std::mutex> lk(mu_);
        const auto deadline = std::chrono::steady_clock::now() + config_.acquireTimeout;
        for (;;) {
            if (!idle_.empty()) {
                detail::PoolSlot* s = idle_.front();
                idle_.pop_front();
                if (config_.validateOnAcquire) {
                    auto v = ensure_alive_locked(s);
                    if (!v.ok()) {
                        remove_slot_locked(s);
                        return v.error();
                    }
                }
                s->last_used_at = config_.now();
                return PooledConnection(this, s);
            }
            if (slots_.size() < config_.max) {
                auto c = make_connection();  // note: holds lock during backoff
                if (!c.ok()) return c.error();
                detail::PoolSlot* s = add_active_slot(std::move(c.value()));
                return PooledConnection(this, s);
            }
            if (cv_.wait_until(lk, deadline) == std::cv_status::timeout &&
                idle_.empty() && slots_.size() >= config_.max) {
                Error e;
                e.code = ErrorCode::Pool;
                e.message = "connection acquire timed out";
                e.retriable = false;
                return e;
            }
        }
    }

    std::size_t total_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return slots_.size();
    }
    std::size_t idle_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return idle_.size();
    }
    std::size_t active_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return slots_.size() - idle_.size();
    }

    // One maintenance pass: reap expired idle connections (respecting min) and
    // refill back up to min. Public so tests can drive it deterministically.
    void maintain();

private:
    ConnectionPool(detail::cli::ICliDriver& driver,
                   detail::cli::ConnectionParams params, PoolConfig config)
        : driver_(&driver), params_(std::move(params)), config_(std::move(config)) {}

    friend class PooledConnection;

    // Attempts a physical connect with bounded exponential backoff.
    Result<std::unique_ptr<Connection>> make_connection() {
        Error last;
        last.code = ErrorCode::Connection;
        last.message = "no connect attempt made";
        const int attempts = config_.backoff.maxAttempts < 1
                                 ? 1
                                 : config_.backoff.maxAttempts;
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            auto c = Connection::open(*driver_, params_);
            if (c.ok())
                return std::make_unique<Connection>(std::move(c.value()));
            last = c.error();
            if (attempt < attempts)
                config_.backoff.sleep(config_.backoff.delay_for(attempt));
        }
        return last;
    }

    detail::PoolSlot* add_active_slot(std::unique_ptr<Connection> conn) {
        auto slot = std::make_unique<detail::PoolSlot>();
        slot->conn = std::move(conn);
        slot->created_at = config_.now();
        slot->last_used_at = config_.now();
        detail::PoolSlot* raw = slot.get();
        slots_.push_back(std::move(slot));
        return raw;
    }

    void add_idle_slot(std::unique_ptr<Connection> conn) {
        idle_.push_back(add_active_slot(std::move(conn)));
    }

    // Validates a slot and reconnects it in place if dead. Caller holds the lock.
    Result<void> ensure_alive_locked(detail::PoolSlot* s) {
        auto a = driver_->isAlive(s->conn->handle());
        if (a.ok() && a.value()) return Result<void>();
        auto c = make_connection();
        if (!c.ok()) return c.error();
        s->conn = std::move(c.value());
        s->created_at = config_.now();
        s->last_used_at = config_.now();
        return Result<void>();
    }

    void remove_slot_locked(detail::PoolSlot* s) {
        for (auto it = slots_.begin(); it != slots_.end(); ++it) {
            if (it->get() == s) {
                slots_.erase(it);
                return;
            }
        }
    }

    void release_slot(detail::PoolSlot* s, bool broken) {
        std::lock_guard<std::mutex> lk(mu_);
        if (broken || stopping_) {
            remove_slot_locked(s);
        } else {
            s->last_used_at = config_.now();
            idle_.push_back(s);
        }
        cv_.notify_one();
    }

    void start_maintenance() {
        maintenance_ = std::thread([this] {
            std::unique_lock<std::mutex> lk(mu_);
            while (!stopping_) {
                maint_cv_.wait_for(lk, config_.maintenanceInterval,
                                   [this] { return stopping_; });
                if (stopping_) break;
                lk.unlock();
                maintain();
                lk.lock();
            }
        });
    }

    detail::cli::ICliDriver* driver_;
    detail::cli::ConnectionParams params_;
    PoolConfig config_;

    mutable std::mutex mu_;
    std::condition_variable cv_;        // signals idle availability to acquirers
    std::condition_variable maint_cv_;  // wakes the reaper to stop
    std::vector<std::unique_ptr<detail::PoolSlot>> slots_;  // all (idle + active)
    std::deque<detail::PoolSlot*> idle_;
    bool stopping_ = false;
    std::thread maintenance_;
};

inline void PooledConnection::release() {
    if (pool_ && slot_) {
        pool_->release_slot(slot_, broken_);
        pool_ = nullptr;
        slot_ = nullptr;
    }
}

// Implemented in Task 4; declared here so the header compiles in Task 2 builds.
inline void ConnectionPool::maintain() {}

}  // namespace halcyon
```

> Note: `maintain()` is a temporary no-op `inline` definition in this task so the
> core compiles and the basics tests pass. Task 4 **replaces** this stub with the
> real reaper (move it out of the class to a single out-of-line definition).
> Concurrency caveat (documented, acceptable for v1): `make_connection()` may run
> backoff sleeps while `mu_` is held, briefly serializing growth. Revisit if the
> integration concurrency test shows contention.

- [ ] **Step 4: Wire into the build (link Threads)**

In `tests/CMakeLists.txt`, ensure the threading library is linked (the pool uses
`std::thread`). Add near the top, after `FetchContent_MakeAvailable(googletest)`:

```cmake
find_package(Threads REQUIRED)
```

and add `Threads::Threads` to the unit test link line:

```cmake
target_link_libraries(halcyon_unit_tests
    PRIVATE halcyon::halcyon GTest::gtest_main Threads::Threads)
```

Also add `unit/test_pool.cpp` to `halcyon_unit_tests`.

- [ ] **Step 5: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R PoolBasics --output-on-failure
```

Expected: all `PoolBasics.*` tests PASS.

- [ ] **Step 6: Commit**

```bash
git add include/halcyon/pool.hpp tests/CMakeLists.txt tests/unit/test_pool.cpp
git commit -m "feat: add thread-safe ConnectionPool with lazy growth and timeout"
```

---

## Task 3: Validation + transparent reconnect

Adds the validate-on-acquire path, reconnect-with-backoff on dead/broken
connections, and the broken-connection discard semantics. The mechanism already
exists in `acquire()`/`ensure_alive_locked()`/`make_connection()` from Task 2;
this task pins the behavior with tests and tunes `make_connection` retries.

**Files:**
- Modify: `tests/unit/test_pool.cpp` (append validation/reconnect tests)
- (No header change expected; if a test fails, fix `pool.hpp` accordingly.)

- [ ] **Step 1: Append the failing tests**

Append to `tests/unit/test_pool.cpp`:

```cpp
TEST(PoolValidation, ReconnectsDeadConnectionOnAcquire) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 2;
    cfg.validateOnAcquire = true;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    EXPECT_EQ(driver.connectCalls, 1);

    driver.aliveResults.push_back(false);  // next validation fails → reconnect
    auto pc = pool->acquire();
    ASSERT_TRUE(pc.ok());
    EXPECT_EQ(driver.connectCalls, 2);     // reconnected in place
    EXPECT_EQ(driver.disconnectCalls, 1);  // old physical handle dropped
    EXPECT_EQ(pool->total_count(), 1u);    // still one logical slot
}

TEST(PoolValidation, HealthyConnectionIsNotReconnected) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.validateOnAcquire = true;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    driver.aliveResults.push_back(true);
    auto pc = pool->acquire();
    ASSERT_TRUE(pc.ok());
    EXPECT_EQ(driver.connectCalls, 1);
}

TEST(PoolBroken, BrokenConnectionIsDiscardedNotReturned) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 0;
    cfg.max = 2;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    {
        auto pc = pool->acquire();
        ASSERT_TRUE(pc.ok());
        pc.value().markBroken();
        EXPECT_EQ(pool->total_count(), 1u);
    }
    EXPECT_EQ(pool->total_count(), 0u);  // discarded on return
    EXPECT_EQ(pool->idle_count(), 0u);
}

TEST(PoolReconnect, RetriesConnectWithBackoff) {
    MockCliDriver driver;
    Error e;
    e.code = ErrorCode::Connection;
    e.sqlstate = "08001";
    e.retriable = true;
    driver.connectErrors.push_back(e);  // fail once
    driver.connectErrors.push_back(e);  // fail twice, then succeed

    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.backoff.maxAttempts = 3;
    cfg.backoff.sleep = [](std::chrono::milliseconds) {};  // no real waiting
    auto pool = ConnectionPool::create(driver, {"x"}, cfg);
    ASSERT_TRUE(pool.ok()) << pool.error().message;
    EXPECT_EQ(driver.connectCalls, 3);  // 2 failures + 1 success
    EXPECT_EQ(pool.value()->total_count(), 1u);
}

TEST(PoolReconnect, ReturnsErrorWhenAllAttemptsFail) {
    MockCliDriver driver;
    Error e;
    e.code = ErrorCode::Connection;
    e.retriable = true;
    for (int i = 0; i < 5; ++i) driver.connectErrors.push_back(e);

    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.backoff.maxAttempts = 2;
    cfg.backoff.sleep = [](std::chrono::milliseconds) {};
    auto pool = ConnectionPool::create(driver, {"x"}, cfg);
    ASSERT_FALSE(pool.ok());
    EXPECT_EQ(pool.error().code, ErrorCode::Connection);
}
```

- [ ] **Step 2: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R "PoolValidation|PoolBroken|PoolReconnect" --output-on-failure
```

Expected: all PASS. If `PoolValidation.ReconnectsDeadConnectionOnAcquire`'s
`disconnectCalls` expectation fails, it means the old `Connection` is not being
destroyed before the new one is assigned in `ensure_alive_locked`; assigning
`s->conn = std::move(c.value())` releases the old `Connection` (calling
`disconnect`) — verify the move-assignment in `pool.hpp` and adjust if needed.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/test_pool.cpp include/halcyon/pool.hpp
git commit -m "feat: validate-on-acquire and transparent reconnect with backoff"
```

---

## Task 4: Background maintenance reaper (idle/lifetime expiry + refill)

Replaces the Task 2 `maintain()` stub with a real pass: reap idle connections
past `idleTimeout` (while above `min`) or past `maxLifetime` (always), then refill
up to `min`. Time is injected through `PoolConfig::now`, so tests advance a fake
clock and call `maintain()` directly (no real sleeping, no thread).

**Files:**
- Modify: `include/halcyon/pool.hpp` (real `maintain()`)
- Modify: `tests/CMakeLists.txt` (add `unit/test_pool_maintenance.cpp`)
- Test: `tests/unit/test_pool_maintenance.cpp`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_pool_maintenance.cpp`:

```cpp
#include <gtest/gtest.h>

#include <chrono>
#include <memory>

#include "halcyon/pool.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Clock;
using halcyon::ConnectionPool;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;
using namespace std::chrono_literals;

namespace {
struct FakeClock {
    std::shared_ptr<Clock::time_point> now =
        std::make_shared<Clock::time_point>(Clock::now());
    void advance(std::chrono::milliseconds d) { *now += d; }
    std::function<Clock::time_point()> fn() const {
        auto n = now;
        return [n] { return *n; };
    }
};
}  // namespace

TEST(PoolMaintenance, ReapsIdleBeyondTimeoutDownToMin) {
    MockCliDriver driver;
    FakeClock clk;
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    cfg.min = 1;
    cfg.max = 4;
    cfg.idleTimeout = 100ms;
    cfg.now = clk.fn();
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();

    { auto a = pool->acquire(); auto b = pool->acquire(); auto c = pool->acquire(); }
    EXPECT_EQ(pool->idle_count(), 3u);

    clk.advance(200ms);
    pool->maintain();
    EXPECT_EQ(pool->total_count(), 1u);  // reaped down to min
    EXPECT_EQ(pool->idle_count(), 1u);
}

TEST(PoolMaintenance, DoesNotReapBelowMin) {
    MockCliDriver driver;
    FakeClock clk;
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    cfg.min = 2;
    cfg.idleTimeout = 100ms;
    cfg.now = clk.fn();
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    EXPECT_EQ(pool->idle_count(), 2u);

    clk.advance(500ms);
    pool->maintain();
    EXPECT_EQ(pool->total_count(), 2u);  // never below min
}

TEST(PoolMaintenance, ReapsBeyondMaxLifetimeEvenAtMin) {
    MockCliDriver driver;
    FakeClock clk;
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    cfg.min = 1;
    cfg.maxLifetime = 100ms;
    cfg.idleTimeout = 1h;  // not the trigger here
    cfg.now = clk.fn();
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    EXPECT_EQ(driver.connectCalls, 1);

    clk.advance(200ms);
    pool->maintain();
    EXPECT_EQ(pool->total_count(), 1u);   // replaced, not dropped
    EXPECT_EQ(driver.connectCalls, 2);    // old reaped + fresh refill
    EXPECT_EQ(driver.disconnectCalls, 1);
}

TEST(PoolMaintenance, RefillsToMinAfterBrokenRemoval) {
    MockCliDriver driver;
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    cfg.min = 2;
    cfg.max = 4;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    {
        auto a = pool->acquire();
        auto b = pool->acquire();
        a.value().markBroken();
    }
    EXPECT_EQ(pool->total_count(), 1u);  // one discarded
    pool->maintain();
    EXPECT_EQ(pool->total_count(), 2u);  // refilled to min
}
```

- [ ] **Step 2: Run test to verify it fails**

Add `unit/test_pool_maintenance.cpp` to `tests/CMakeLists.txt`, reconfigure,
build. Expected: tests FAIL (the Task 2 `maintain()` is a no-op).

- [ ] **Step 3: Implement the real `maintain()`**

In `include/halcyon/pool.hpp`, delete the stub line
`inline void ConnectionPool::maintain() {}` and replace it with the full
out-of-line definition (place it after `PooledConnection::release` so all members
are complete):

```cpp
inline void ConnectionPool::maintain() {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = config_.now();

    std::deque<detail::PoolSlot*> keep;
    for (detail::PoolSlot* s : idle_) {
        const bool expired_life = (now - s->created_at) >= config_.maxLifetime;
        const bool expired_idle = (now - s->last_used_at) >= config_.idleTimeout;
        const bool over_min = slots_.size() > config_.min;
        if (expired_life || (expired_idle && over_min)) {
            remove_slot_locked(s);  // Connection dtor disconnects
        } else {
            keep.push_back(s);
        }
    }
    idle_ = std::move(keep);

    // Refill up to min (best effort; a failed connect is retried next pass).
    while (slots_.size() < config_.min) {
        auto c = make_connection();
        if (!c.ok()) break;
        add_idle_slot(std::move(c.value()));
    }
}
```

> Note: `maintain()` iterates `idle_` while `remove_slot_locked` mutates `slots_`
> (a different container), so the iterated `PoolSlot*` in `keep` stay valid. Only
> slots being reaped are erased. `make_connection()` runs under the lock here too,
> matching the Task 2 caveat.

- [ ] **Step 4: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R PoolMaintenance --output-on-failure
```

Expected: all `PoolMaintenance.*` tests PASS, and the earlier `PoolBasics.*` /
`PoolValidation.*` / `PoolBroken.*` / `PoolReconnect.*` still PASS.

- [ ] **Step 5: Commit**

```bash
git add include/halcyon/pool.hpp tests/CMakeLists.txt \
        tests/unit/test_pool_maintenance.cpp
git commit -m "feat: add pool maintenance reaper with idle/lifetime expiry and refill"
```

---

## Task 5: Async executor (`async.hpp`)

A fixed-size worker pool with a single `submit(callable) -> std::future` chokepoint
(so a C++20 coroutine layer can wrap it later) plus `async_with_connection`, which
acquires a pooled connection on a worker and runs `fn(Connection&)`.

**Files:**
- Create: `include/halcyon/async.hpp`
- Modify: `tests/CMakeLists.txt` (add `unit/test_async.cpp`)
- Test: `tests/unit/test_async.cpp`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_async.cpp`:

```cpp
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <vector>

#include "halcyon/async.hpp"
#include "halcyon/pool.hpp"
#include "halcyon/result.hpp"
#include "mock_cli_driver.hpp"

using halcyon::ConnectionPool;
using halcyon::Executor;
using halcyon::PoolConfig;
using halcyon::Result;
using halcyon::testing::MockCliDriver;

TEST(Executor, SubmitReturnsFutureWithValue) {
    Executor ex(2);
    auto f = ex.submit([] { return 6 * 7; });
    EXPECT_EQ(f.get(), 42);
}

TEST(Executor, RunsManyTasks) {
    Executor ex(4);
    std::vector<std::future<int>> fs;
    for (int i = 0; i < 16; ++i) fs.push_back(ex.submit([i] { return i; }));
    int sum = 0;
    for (auto& f : fs) sum += f.get();
    EXPECT_EQ(sum, 120);
}

TEST(Executor, DrainsOutstandingTasksOnDestruction) {
    std::atomic<int> done{0};
    {
        Executor ex(2);
        for (int i = 0; i < 8; ++i)
            ex.submit([&done] { done.fetch_add(1); });
    }  // dtor joins after draining the queue
    EXPECT_EQ(done.load(), 8);
}

TEST(AsyncWithConnection, RunsQueryOnPooledConnection) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"}, {{halcyon::detail::cli::Value{std::int64_t{5}}},
                 {halcyon::detail::cli::Value{std::int64_t{9}}}}});
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    Executor ex(2);

    auto f = halcyon::async_with_connection(
        ex, *pool, [](halcyon::Connection& c) -> Result<std::int64_t> {
            auto rs = c.query("SELECT id FROM t");
            if (!rs.ok()) return rs.error();
            std::int64_t sum = 0;
            for (auto& row : rs.value()) sum += std::get<0>(row.as<std::int64_t>());
            return sum;
        });
    auto r = f.get();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 14);
}
```

- [ ] **Step 2: Run test to verify it fails**

Add `unit/test_async.cpp` to `tests/CMakeLists.txt`, reconfigure, build. Expected:
compile FAIL — `halcyon/async.hpp` not found.

- [ ] **Step 3: Write the async header**

Create `include/halcyon/async.hpp`:

```cpp
#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "halcyon/connection.hpp"
#include "halcyon/pool.hpp"

namespace halcyon {

// Fixed-size thread pool with a single submit() chokepoint. A future coroutine
// task<T>/awaitable layer can wrap submit() without changing this API.
class Executor {
public:
    explicit Executor(std::size_t threads) {
        if (threads == 0) threads = 1;
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i)
            workers_.emplace_back([this] { worker_loop(); });
    }

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    ~Executor() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
    }

    template <class Fn>
    auto submit(Fn&& fn) -> std::future<std::invoke_result_t<std::decay_t<Fn>>> {
        using R = std::invoke_result_t<std::decay_t<Fn>>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<Fn>(fn));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(mu_);
            tasks_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                if (tasks_.empty()) {
                    if (stop_) return;
                    continue;
                }
                job = std::move(tasks_.front());
                tasks_.pop();
            }
            job();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
};

// Acquires a pooled connection on a worker thread and runs fn(Connection&). fn
// must return a Result<T>; an acquire failure short-circuits to that error.
template <class Fn>
auto async_with_connection(Executor& ex, ConnectionPool& pool, Fn fn)
    -> std::future<std::decay_t<decltype(fn(std::declval<Connection&>()))>> {
    using R = std::decay_t<decltype(fn(std::declval<Connection&>()))>;
    return ex.submit([&pool, fn = std::move(fn)]() mutable -> R {
        auto pc = pool.acquire();
        if (!pc.ok()) return R(pc.error());
        return fn(*pc.value());
    });
}

}  // namespace halcyon
```

> Note on draining: workers exit only once the queue is empty *and* `stop_` is
> set, so all submitted tasks run before `~Executor` returns (the
> `Executor.DrainsOutstandingTasksOnDestruction` test pins this).

- [ ] **Step 4: Wire into the build**

Add `unit/test_async.cpp` to `halcyon_unit_tests` in `tests/CMakeLists.txt`.
(`Threads::Threads` was already linked in Task 2.)

- [ ] **Step 5: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R "Executor|AsyncWithConnection" --output-on-failure
```

Expected: all async tests PASS.

- [ ] **Step 6: Commit**

```bash
git add include/halcyon/async.hpp tests/CMakeLists.txt tests/unit/test_async.cpp
git commit -m "feat: add std::future Executor and async_with_connection helper"
```

---

## Task 6: Umbrella header + gated pool integration test

Exposes the new headers via the umbrella and adds a live-DB concurrency test that
runs only when `HALCYON_TEST_DSN` is set (skipped otherwise, like Plan 2).

**Files:**
- Modify: `include/halcyon/halcyon.hpp`
- Modify: `tests/integration/test_db2_roundtrip.cpp`

- [ ] **Step 1: Extend the umbrella header**

Edit `include/halcyon/halcyon.hpp` to add the pool/async/retry headers:

```cpp
#pragma once

// Halcyon — modern C++17 IBM Db2 client. Umbrella header.
#include "halcyon/async.hpp"
#include "halcyon/connection.hpp"
#include "halcyon/error.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/pool.hpp"
#include "halcyon/result.hpp"
#include "halcyon/retry.hpp"
#include "halcyon/types.hpp"
#include "halcyon/version.hpp"
#include "halcyon/detail/cli/db2_cli_driver.hpp"
```

- [ ] **Step 2: Add a gated pool concurrency test**

Append to `tests/integration/test_db2_roundtrip.cpp` (it already includes
`halcyon/halcyon.hpp` and defines the `dsn()` helper):

```cpp
#include <thread>
#include <vector>

TEST(Db2PoolIntegration, ConcurrentAcquireAndQuery) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";

    auto driver = halcyon::detail::cli::make_db2_cli_driver();
    halcyon::PoolConfig cfg;
    cfg.min = 2;
    cfg.max = 6;
    cfg.validateOnAcquire = true;
    auto pool = halcyon::ConnectionPool::create(*driver, {*d}, cfg);
    ASSERT_TRUE(pool.ok()) << pool.error().message;

    halcyon::Executor ex(6);
    std::vector<std::future<halcyon::Result<std::int64_t>>> futs;
    for (int i = 0; i < 24; ++i) {
        futs.push_back(halcyon::async_with_connection(
            ex, *pool.value(),
            [](halcyon::Connection& c) -> halcyon::Result<std::int64_t> {
                auto rs = c.query("SELECT 1 FROM SYSIBM.SYSDUMMY1");
                if (!rs.ok()) return rs.error();
                std::int64_t n = 0;
                for (auto& row : rs.value()) n += std::get<0>(row.as<std::int64_t>());
                return n;
            }));
    }
    for (auto& f : futs) {
        auto r = f.get();
        ASSERT_TRUE(r.ok()) << r.error().message;
        EXPECT_EQ(r.value(), 1);
    }
}
```

- [ ] **Step 3: Build everything (no DB needed to compile)**

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON -DHALCYON_BUILD_INTEGRATION_TESTS=ON
cmake --build build -j
```

Expected: unit + integration targets build. The new integration test is reported
**skipped** without `HALCYON_TEST_DSN`.

- [ ] **Step 4: Run the full unit suite**

```bash
ctest --test-dir build --output-on-failure
```

Expected: every unit test from Plans 1–3 PASSES.

- [ ] **Step 5: (Optional) Run integration against live Db2**

```bash
docker compose -f docker/docker-compose.yml up -d
# wait for healthy, then:
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
ctest --test-dir build -L integration --output-on-failure
docker compose -f docker/docker-compose.yml down
```

Expected: `Db2Integration.*` and `Db2PoolIntegration.*` PASS against live Db2.

- [ ] **Step 6: Commit**

```bash
git add include/halcyon/halcyon.hpp tests/integration/test_db2_roundtrip.cpp
git commit -m "test: expose pool/async via umbrella and add gated pool concurrency test"
```

---

## Self-Review

**Spec coverage (Plan 3 scope, spec §8):**
- `PoolConfig` (`min`/`max`/`acquireTimeout`/`idleTimeout`/`maxLifetime`/
  `validateOnAcquire`/reconnect backoff) → Task 2 + 4. ✔ (`validationQuery` is
  delegated to the driver's `isAlive()` per the seam; see Deferred.)
- Mutex + condition_variable guarding an idle deque + total/active counts → Task 2. ✔
- `acquire()` returns a `PooledConnection` RAII handle that returns the connection
  on destruction (or discards if broken) → Tasks 2–3. ✔
- Lazy growth `min`→`max`; blocks up to `acquireTimeout`, then `Pool` error → Task 2. ✔
- Background maintenance reaps idle/over-lifetime connections and refills to
  `min` → Task 4. ✔
- Validation on acquire + transparent reconnect with bounded exponential backoff;
  poisoned connections never returned → Task 3. ✔
- Safe auto-retry primitives (`ExecPolicy`, `with_retry`, read-only
  classification) → Task 1. ✔
- Async executor returning `std::future<Result<T>>` with a single `submit`
  chokepoint, coroutine-ready → Task 5. ✔
- Thread-safety contract: `ConnectionPool` fully thread-safe (all state under
  `mu_`); a single `Connection`/`ResultSet` stays single-owner — the pool hands a
  slot to exactly one `PooledConnection` at a time. ✔
- Mockability invariant preserved: the whole layer is tested via `MockCliDriver`;
  no `sqlcli1.h`. ✔

**Type consistency:** `BackoffPolicy`/`ExecPolicy`/`with_retry` (Task 1) feed
`ConnectionPool::make_connection` (Task 2) and `async_with_connection` callers.
`PoolConfig::now` (`Clock` = `steady_clock`) drives both `acquire` bookkeeping and
the reaper, and is overridden by the `FakeClock` in Task 4. `Connection::open`/
`handle`/`query` from Plan 2 are used unchanged by the slot lifecycle. `ErrorCode::
Pool` (Plan 1) is the acquire-timeout code; `Error.retriable` (Plan 1) gates
`with_retry`. `PooledConnection::operator->` returns the Plan 2 `Connection`,
so `query`/`execute`/`queryAs` work directly on a leased connection.

**Determinism:** No test sleeps for real time — backoff sleeps are injected
no-ops and the reaper uses an injected fake clock with `startMaintenanceThread =
false` + direct `maintain()` calls. The only real-time wait is the intentional
`PoolBasics.AcquireTimesOutWhenExhausted` (30 ms) using the default clock.

**Deferred (intentionally, plug into the same patterns later):**
- Wiring auto-retry through user-facing `query`/`execute` (the facade decides
  idempotency via `detail::is_read_only` + `ExecPolicy`) → Plan 4.
- Auto-detecting connection-class errors during use to call `markBroken()`
  automatically (Plan 4 facade wraps calls; Plan 3 exposes the manual hook).
- Per-connection prepared-statement caching (spec non-goal for v1).
- Releasing `mu_` during reconnect backoff sleeps (documented v1 caveat).
- `Database` facade, RAII `Transaction`, bulk/batch, observability → Plans 4–5.

**Placeholder scan:** No TBD/TODO/"handle errors here" — every code step is
complete. One step (Task 2 Step 3) intentionally ships a temporary no-op
`maintain()` stub that Task 4 Step 3 **replaces** with the real reaper, and says
so explicitly.

---

## Roadmap: subsequent plans (unchanged from Plans 1–2)

- **Plan 4 — Facade & ergonomics:** `Database::open`/`openOrThrow`, OO `query`/
  `execute`/`queryAs`, functional free functions, RAII `Transaction` + functional
  `transaction(...)`, `executeBatch`/`batchOf` bulk path, auto-retry wiring
  (`ExecPolicy` + `is_read_only`), `queryAsync`/`executeAsync` on `Database`,
  examples.
- **Plan 5 — Observability:** `MetricsSink`/`Tracer` no-op defaults, `prometheus-cpp`
  and `opentelemetry-cpp` adapters behind `HALCYON_WITH_PROMETHEUS`/`HALCYON_WITH_OTEL`,
  metric/span emission at query/transaction/pool boundaries (`halcyon_pool_*`,
  `halcyon_reconnects_total`, `halcyon_retries_total`).
