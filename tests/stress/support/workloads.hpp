#pragma once

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
            return;  // timeout under contention is acceptable
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

}  // namespace halcyon::stress
