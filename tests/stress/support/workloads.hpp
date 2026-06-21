#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "halcyon/database.hpp"
#include "halcyon/pool.hpp"
#include "halcyon/types.hpp"
#include "concurrent_fake_driver.hpp"
#include "workload_runner.hpp"

namespace halcyon::stress {

// Single-column scalar row used by every scalar SELECT in the workloads. The fake
// (and live Db2) return one int64 column equal to the integer embedded in the SQL.
struct One {
    std::int64_t v;
};

}  // namespace halcyon::stress

HALCYON_REFLECT(halcyon::stress::One, v);

namespace halcyon::stress {

enum class ScenarioId { Pool, Executor, Cache, Reconnect, Txn, Lifecycle };

// The tuned PoolConfig for each scenario. pool_max is the only sweepable knob the
// perf harness varies; everything else is scenario-intrinsic.
inline PoolConfig config_for(ScenarioId id, std::size_t pool_max) {
    using namespace std::chrono_literals;
    PoolConfig cfg;
    cfg.max = pool_max == 0 ? 1 : pool_max;
    cfg.min = cfg.max < 2 ? cfg.max : 2;
    cfg.startMaintenanceThread = false;
    cfg.statementCacheSize = 0;
    cfg.backoff.sleep = [](std::chrono::milliseconds) {};  // never really wait
    switch (id) {
        case ScenarioId::Pool:
            cfg.acquireTimeout = 50ms;
            break;
        case ScenarioId::Executor:
            cfg.statementCacheSize = 16;
            break;
        case ScenarioId::Cache:
            cfg.min = 1;
            cfg.max = 2;
            cfg.statementCacheSize = 8;
            break;
        case ScenarioId::Reconnect:
            cfg.validateOnAcquire = true;
            cfg.backoff.maxAttempts = 3;
            cfg.statementCacheSize = 8;
            break;
        case ScenarioId::Txn:
            break;
        case ScenarioId::Lifecycle:
            cfg.startMaintenanceThread = true;
            cfg.maintenanceInterval = 1ms;
            cfg.idleTimeout = 1ms;
            cfg.maxLifetime = 5ms;
            break;
    }
    return cfg;
}

// A canonical scalar SELECT whose result the fake/live encode as `n`.
inline std::string select_n(std::int64_t n) {
    return "SELECT " + std::to_string(n) + " FROM SYSIBM.SYSDUMMY1";
}

// --- Scenario 1: raw pool acquire/release contention (pool mechanics only) ---
// Acquire a pooled connection, run one scalar query directly on it (no facade
// retry), release via RAII scope. A pool-timeout error is expected under
// contention and is NOT a failure.
inline Workload make_pool_contention(Database& db) {
    return Workload{"pool", [&db](WorkerCtx& ctx) {
        auto pc = db.pool().acquire();
        if (!pc.ok()) {
            if (pc.error().code != ErrorCode::Pool)
                ctx.fail("unexpected acquire error: " + pc.error().message);
            else
                ctx.note_error();  // tolerated acquire-timeout under contention
            return;
        }
        auto r = pc.value()->queryAs<One>(select_n(7));
        if (!r.ok()) {
            ctx.fail("query failed: " + r.error().message);
            return;
        }
        if (r.value().size() != 1 || r.value()[0].v != 7)
            ctx.fail("wrong scalar from pooled query");
    }};
}

// --- Scenario 2: async executor saturation (product path: Database async) ---
// Each op launches a typed async query on the Database's internal executor and
// blocks on its future. Many runner threads launching at once saturate the fixed
// executor; every future must resolve exactly once with the right scalar.
inline Workload make_executor_saturation(Database& db) {
    return Workload{"executor", [&db](WorkerCtx& ctx) {
        auto fut = db.queryAsync<One>(select_n(7));
        auto r = fut.get();
        if (!r.ok()) {
            ctx.fail("async query failed: " + r.error().message);
            return;
        }
        if (r.value().size() != 1 || r.value()[0].v != 7)
            ctx.fail("wrong scalar from async query");
    }};
}

// --- Scenario 3: statement-cache correctness under reuse ---
// A small rotating set of SQL strings on few connections (cache enabled) so hits,
// the busy->overflow path (same SQL concurrently), and eviction all occur. Each op
// asserts the scalar matches the SQL it sent (proves no cross-thread row mixup).
inline Workload make_cache_churn(Database& db) {
    return Workload{"cache", [&db](WorkerCtx& ctx) {
        std::int64_t n = 1 + static_cast<std::int64_t>(ctx.rng() % 3);  // 1..3
        auto r = db.queryAs<One>(select_n(n));
        if (!r.ok()) {
            ctx.fail("cache query failed: " + r.error().message);
            return;
        }
        if (r.value().size() != 1 || r.value()[0].v != n)
            ctx.fail("wrong scalar from cached query");
    }};
}

// --- Scenario 4: reconnect + transient-fault injection under load ---
// Driven through the facade so the safe-retry policy is in play. With fault knobs
// set on the driver and validateOnAcquire on the pool, read-only queries are
// transparently retried / reconnected. The safe-retry policy is BOUNDED (spec
// §6.4: recovery happens "per the safe-retry policy"), so under adversarial 1-in-N
// fault injection a minority of ops can legitimately exhaust their attempts and
// surface the still-RETRIABLE error — that is correct policy behaviour, not a bug.
// What must NEVER happen is a non-retriable surfaced error or a wrong scalar (a
// cross-thread handle/row mixup). `recovered`, if provided, counts ops that came
// back with the right value, so the caller can prove the bulk genuinely recovered.
inline Workload make_reconnect_faults(Database& db,
                                      std::atomic<long>* recovered = nullptr) {
    return Workload{"reconnect", [&db, recovered](WorkerCtx& ctx) {
        auto r = db.queryAs<One>(select_n(7));
        if (!r.ok()) {
            // Exhausting a bounded retry budget surfaces a retriable error: allowed.
            // A non-retriable error means recovery corrupted the path: a failure.
            if (!r.error().retriable)
                ctx.fail("non-retriable error surfaced: " + r.error().message);
            else
                ctx.note_error();  // tolerated: bounded retry genuinely exhausted
            return;
        }
        if (r.value().size() != 1 || r.value()[0].v != 7) {
            ctx.fail("wrong scalar after recovery");  // cross-thread handle mixup
            return;
        }
        if (recovered) recovered->fetch_add(1, std::memory_order_relaxed);
    }};
}

// --- Scenario 5: transaction churn (facade transaction(); commit + rollback) ---
// Half the ops commit (lambda returns ok) and half roll back (lambda returns an
// error Result), exercising ScopedTransaction's commit/rollback + lease-return.
inline Workload make_txn_churn(Database& db) {
    return Workload{"txn", [&db](WorkerCtx& ctx) {
        const bool do_commit = (ctx.rng() & 1u) == 0u;
        auto r = db.transaction(
            [&](Transaction& tx) -> Result<std::int64_t> {
                auto q = tx.queryAs<One>(select_n(7));
                if (!q.ok()) return q.error();
                if (q.value().size() != 1 || q.value()[0].v != 7) {
                    Error e;
                    e.code = ErrorCode::Mapping;
                    e.message = "wrong scalar in tx";
                    return e;
                }
                if (do_commit) return std::int64_t{1};
                Error e;          // force a rollback path
                e.code = ErrorCode::Unknown;
                e.message = "intentional rollback";
                return e;
            });
        // A committed tx returns ok; a rolled-back tx returns our forced error.
        // A Mapping error means the in-tx read was wrong -> real failure.
        if (!r.ok() && r.error().code == ErrorCode::Mapping)
            ctx.fail("in-transaction read returned wrong scalar");
    }};
}

}  // namespace halcyon::stress
