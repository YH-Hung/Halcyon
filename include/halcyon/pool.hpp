#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "halcyon/connection.hpp"
#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/error.hpp"
#include "halcyon/isolation.hpp"
#include "halcyon/observability/config.hpp"
#include "halcyon/observability/instrument.hpp"
#include "halcyon/result.hpp"
#include "halcyon/retry.hpp"

namespace halcyon {

using Clock = std::chrono::steady_clock;

/// \brief Configuration for a `ConnectionPool` (sizing, timeouts, observability).
///
/// Pass to `Database::open` or `ConnectionPool::create`. All fields have
/// reasonable defaults: 1–8 connections, 5 s acquire timeout, 60 s idle
/// timeout, 30 min max lifetime, no observability.
struct PoolConfig {
    std::size_t min = 1;
    std::size_t max = 8;
    std::chrono::milliseconds acquireTimeout{5000};
    std::chrono::milliseconds idleTimeout{60000};
    std::chrono::milliseconds maxLifetime{1800000};
    bool validateOnAcquire = false;
    BackoffPolicy backoff{};  // reconnect backoff
    std::function<Clock::time_point()> now = [] { return Clock::now(); };
    std::chrono::milliseconds maintenanceInterval{1000};
    bool startMaintenanceThread = true;
    obs::ObservabilityConfig observability{};  // default-null = no-op
    std::size_t statementCacheSize = 64;       // per-connection prepared-stmt LRU; 0 disables
    std::optional<Isolation> isolation;        // session default; nullopt = server default (CS)
};

/// \brief Snapshot of pool health counters (v1.2), taken under a single lock
/// acquisition by ConnectionPool::stats() / Database::poolStats(): within one
/// snapshot `busy <= size` and `idle + busy == size` always hold.
struct PoolStats {
    // point-in-time
    std::size_t size;  // connections currently owned (idle + busy)
    std::size_t idle;
    std::size_t busy;
    std::size_t waiters;   // threads blocked in acquire()
    std::size_t peakBusy;  // high-water mark since construction
    // monotonic since pool construction
    std::uint64_t createdTotal;
    std::uint64_t acquiredTotal;
    std::uint64_t acquireTimeoutsTotal;
    std::uint64_t reapedIdleTotal;
    std::uint64_t reapedLifetimeTotal;
    std::uint64_t discardedTotal;  // poisoned + validation discards (a dead
                                   // connection replaced by transparent
                                   // reconnect counts: its predecessor was
                                   // discarded)
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

/// \brief Thread-safe connection pool with lazy growth, blocking acquire, and transparent reconnect.
///
/// Created via `ConnectionPool::create`; non-copyable, non-movable (owns a mutex
/// and maintenance thread). `Database` manages one internally — prefer `Database`
/// for application code.
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
        // Emit the initial pool gauge once after warmup (single-threaded here;
        // no other thread can observe the pool yet, so no lock is needed).
        PendingMetrics pm;
        pool->snapshot_gauge_locked(pm);
        pool->flush_metrics(pm);
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
        // Time the wait and trace the acquire (both no-ops unless instrumented).
        // startSpan runs before the lock; all metric/sink emission is deferred to
        // flush_metrics() after the lock is released (see PendingMetrics).
        detail::obs::Timer timer;
        obs::ScopedSpan span = has_tracer_
                                   ? obs::ScopedSpan(tracer_->startSpan(
                                         "halcyon.acquire", {}))
                                   : obs::ScopedSpan();
        PendingMetrics pm;
        bool timed_out = false;
        Result<PooledConnection> result = [&]() -> Result<PooledConnection> {
            std::unique_lock<std::mutex> lk(mu_);
            const auto deadline =
                std::chrono::steady_clock::now() + config_.acquireTimeout;
            for (;;) {
                if (!idle_.empty()) {
                    detail::PoolSlot* s = idle_.front();
                    idle_.pop_front();
                    if (config_.validateOnAcquire) {
                        // Validates (reconnecting if dead) WITHOUT mu_ held; s is
                        // ours (popped) but stays in slots_ throughout.
                        auto v = ensure_alive(lk, s, pm);
                        if (!v.ok()) {
                            ++discardedTotal_;  // validation-failed discard
                            remove_slot_locked(s);
                            snapshot_gauge_locked(pm);
                            cv_.notify_one();  // capacity freed → wake a waiter
                            return v.error();
                        }
                    }
                    s->last_used_at = config_.now();
                    note_acquired_locked();
                    snapshot_gauge_locked(pm);  // idle -> active
                    pm.acquireWait = true;
                    return PooledConnection(this, s);
                }
                if (slots_.size() + pending_ < config_.max) {
                    // Reserve capacity (pending_), then connect WITHOUT mu_ held
                    // so a slow Db2 connect / backoff sleep can't block other
                    // acquirers, releasers, or the destructor. pending_ keeps
                    // concurrent acquirers from collectively exceeding max.
                    ++pending_;
                    lk.unlock();
                    auto c = make_connection();
                    lk.lock();
                    --pending_;
                    if (!c.ok()) {
                        cv_.notify_one();  // capacity freed → wake a waiter
                        return c.error();
                    }
                    detail::PoolSlot* s = add_active_slot(std::move(c.value()));
                    note_acquired_locked();
                    snapshot_gauge_locked(pm);  // final state: active (leased out)
                    pm.acquireWait = true;
                    return PooledConnection(this, s);
                }
                ++waiters_;
                const auto waitStatus = cv_.wait_until(lk, deadline);
                --waiters_;
                if (waitStatus == std::cv_status::timeout &&
                    idle_.empty() && slots_.size() + pending_ >= config_.max) {
                    Error e;
                    e.code = ErrorCode::Pool;
                    e.message = "connection acquire timed out";
                    e.retriable = false;
                    pm.acquireWait = true;
                    pm.exhausted = true;
                    ++acquireTimeoutsTotal_;
                    timed_out = true;
                    return e;
                }
            }
        }();  // mu_ released here
        pm.acquireSecs = timer.elapsed_seconds();
        if (timed_out) span.setStatusError("");
        flush_metrics(pm);
        return result;
    }

    // One internally-consistent snapshot under the pool mutex (v1.2):
    // busy <= size and idle + busy == size always hold within a snapshot.
    PoolStats stats() const {
        std::lock_guard<std::mutex> lk(mu_);
        PoolStats s;
        s.size = slots_.size();
        s.idle = idle_.size();
        s.busy = s.size - s.idle;
        s.waiters = waiters_;
        s.peakBusy = peakBusy_;
        s.createdTotal = createdTotal_.load(std::memory_order_relaxed);
        s.acquiredTotal = acquiredTotal_;
        s.acquireTimeoutsTotal = acquireTimeoutsTotal_;
        s.reapedIdleTotal = reapedIdleTotal_;
        s.reapedLifetimeTotal = reapedLifetimeTotal_;
        s.discardedTotal = discardedTotal_;
        return s;
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

    // The reconnect backoff policy this pool was configured with (used by the
    // facade to derive a default per-call ExecPolicy).
    BackoffPolicy backoff_policy() const {
        std::lock_guard<std::mutex> lk(mu_);
        return config_.backoff;
    }

    // Observability sinks (resolved once at construction; never null). The
    // facade reads these to instrument query/execute on the same sinks the pool
    // uses for acquire/reconnect/gauge.
    obs::MetricsSink* metrics() const noexcept { return metrics_; }
    obs::Tracer* tracer() const noexcept { return tracer_; }
    bool metrics_enabled() const noexcept { return has_metrics_; }
    bool tracer_enabled() const noexcept { return has_tracer_; }

    // Nullable structured-log sink (resolved once at construction); the facade
    // reads this so retry events land on the same logger the pool uses.
    obs::ILogger* logger() const noexcept { return logger_; }

    // One maintenance pass: reap expired idle connections (respecting min) and
    // refill back up to min. Public so tests can drive it deterministically.
    void maintain();

private:
    ConnectionPool(detail::cli::ICliDriver& driver,
                   detail::cli::ConnectionParams params, PoolConfig config)
        : driver_(&driver), params_(std::move(params)), config_(std::move(config)) {
        // Resolve null sinks to the process-wide no-ops and cache "is real"
        // flags so every emission site is one predictable branch with no
        // allocation on the default (uninstrumented) path. The shared_ptrs in
        // config_.observability keep any real sinks alive for the pool's life.
        metrics_ = config_.observability.metrics ? config_.observability.metrics.get()
                                                 : &obs::noop_metrics_sink();
        tracer_ = config_.observability.tracer ? config_.observability.tracer.get()
                                               : &obs::noop_tracer();
        has_metrics_ = static_cast<bool>(config_.observability.metrics);
        has_tracer_ = static_cast<bool>(config_.observability.tracer);
        logger_ = config_.observability.logger.get();
    }

    friend class PooledConnection;

    // Observability emissions deferred out of the critical section: the locked
    // code records what to emit into one of these, and flush_metrics() calls the
    // user-provided sinks AFTER mu_ is released. This keeps a re-entrant, locking,
    // or slow sink from deadlocking or stalling acquire/release/maintain, and
    // guarantees the gauge is sampled at a single consistent state.
    struct PendingMetrics {
        bool gauge = false;
        double idle = 0;
        double active = 0;
        bool acquireWait = false;
        double acquireSecs = 0;
        int reconnects = 0;
        bool exhausted = false;  // acquire timed out -> log pool.exhausted
        int reaped = 0;          // maintain() removals -> log pool.reap
    };

    // Records the current idle/active counts. Caller holds mu_ (or is the
    // single-threaded constructor), so the snapshot is a consistent point-in-time.
    void snapshot_gauge_locked(PendingMetrics& pm) const {
        pm.gauge = true;
        pm.idle = static_cast<double>(idle_.size());
        pm.active = static_cast<double>(slots_.size()) - pm.idle;
    }

    // Caller holds mu_. Records a successful lease-out for the stats counters.
    void note_acquired_locked() {
        ++acquiredTotal_;
        const std::size_t busy = slots_.size() - idle_.size();
        if (busy > peakBusy_) peakBusy_ = busy;
    }

    // Emits everything recorded in pm. MUST be called with mu_ NOT held.
    void flush_metrics(const PendingMetrics& pm) {
        if (has_metrics_) {
            if (pm.gauge) {
                metrics_->gauge("halcyon_pool_connections", pm.idle,
                                {{"state", "idle"}});
                metrics_->gauge("halcyon_pool_connections", pm.active,
                                {{"state", "active"}});
            }
            if (pm.acquireWait)
                metrics_->histogram("halcyon_pool_acquire_wait_seconds",
                                    pm.acquireSecs, {});
            if (pm.reconnects > 0)
                metrics_->counter("halcyon_reconnects_total",
                                  static_cast<double>(pm.reconnects), {});
        }
        if (has_tracer_)
            for (int i = 0; i < pm.reconnects; ++i)
                tracer_->startSpan("halcyon.reconnect", {})->end();
        if (logger_ != nullptr) {
            if (pm.exhausted)
                logger_->log(obs::LogLevel::Warn, "pool.exhausted",
                             {{"timeout_ms",
                               static_cast<std::int64_t>(
                                   config_.acquireTimeout.count())}});
            if (pm.reaped > 0)
                logger_->log(obs::LogLevel::Debug, "pool.reap",
                             {{"count", pm.reaped}});
        }
    }

    // Attempts a physical connect with bounded exponential backoff.
    Result<std::unique_ptr<Connection>> make_connection() {
        Error last;
        last.code = ErrorCode::Connection;
        last.message = "no connect attempt made";
        const int attempts = config_.backoff.maxAttempts < 1
                                 ? 1
                                 : config_.backoff.maxAttempts;
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            auto c = Connection::open(*driver_, params_,
                                      config_.statementCacheSize,
                                      has_metrics_ ? metrics_ : nullptr,
                                      logger_);
            if (c.ok() && config_.isolation) {
                auto iso = c.value().setDefaultIsolation(*config_.isolation);
                if (!iso.ok()) c = iso.error();  // treat as a failed attempt
            }
            if (c.ok()) {
                if (logger_ != nullptr)
                    logger_->log(obs::LogLevel::Info, "connect.ok",
                                 {{"attempt", attempt}});
                createdTotal_.fetch_add(1, std::memory_order_relaxed);
                return std::make_unique<Connection>(std::move(c.value()));
            }
            last = c.error();
            if (logger_ != nullptr)
                logger_->log(obs::LogLevel::Error, "connect.fail",
                             {{"attempt", attempt},
                              {"code", to_string(last.code)},
                              {"sqlstate", last.sqlstate}});
            if (attempt < attempts)
                config_.backoff.sleep(config_.backoff.delay_for(attempt));
        }
        return last;
    }

    // Inserts a slot into slots_ WITHOUT emitting the gauge; each caller emits
    // exactly once after the slot reaches its final idle/active state, so a
    // record-every-sample sink never sees a transient miscount.
    detail::PoolSlot* add_active_slot(std::unique_ptr<Connection> conn) {
        auto slot = std::make_unique<detail::PoolSlot>();
        slot->conn = std::move(conn);
        slot->created_at = config_.now();
        slot->last_used_at = config_.now();
        detail::PoolSlot* raw = slot.get();
        slots_.push_back(std::move(slot));
        return raw;
    }

    // Bookkeeping only; the caller snapshots the gauge once at its final state.
    void add_idle_slot(std::unique_ptr<Connection> conn) {
        idle_.push_back(add_active_slot(std::move(conn)));
    }

    // Validates a slot and reconnects it in place if dead. Drops mu_ (via lk) for
    // the isAlive probe and the reconnect so a slow/dead Db2 connection can't
    // block other callers; s is caller-owned (already popped from idle_) and
    // stays in slots_ across the gap, so it can't be reaped meanwhile. The lock
    // is always re-held on return. A reconnect is RECORDED into pm and emitted
    // after the lock is released.
    Result<void> ensure_alive(std::unique_lock<std::mutex>& lk,
                              detail::PoolSlot* s, PendingMetrics& pm) {
        lk.unlock();
        auto a = driver_->isAlive(s->conn->handle());
        if (a.ok() && a.value()) {
            lk.lock();
            return Result<void>();
        }
        if (logger_ != nullptr)
            logger_->log(obs::LogLevel::Warn, "reconnect.attempt", {});
        auto c = make_connection();
        if (logger_ != nullptr) {
            if (c.ok())
                logger_->log(obs::LogLevel::Info, "reconnect.ok", {});
            else
                logger_->log(obs::LogLevel::Error, "reconnect.fail",
                             {{"code", to_string(c.error().code)}});
        }
        lk.lock();
        if (!c.ok()) return c.error();
        s->conn = std::move(c.value());
        s->created_at = config_.now();
        s->last_used_at = config_.now();
        ++pm.reconnects;    // transparently reconnected in place
        ++discardedTotal_;  // the dead original was discarded and replaced
        return Result<void>();
    }

    // Bookkeeping only (no emission); the caller snapshots the gauge afterward.
    void remove_slot_locked(detail::PoolSlot* s) {
        for (auto it = slots_.begin(); it != slots_.end(); ++it) {
            if (it->get() == s) {
                slots_.erase(it);
                return;
            }
        }
    }

    void release_slot(detail::PoolSlot* s, bool broken) {
        PendingMetrics pm;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (broken || stopping_) {
                if (broken) ++discardedTotal_;
                remove_slot_locked(s);
            } else {
                s->last_used_at = config_.now();
                idle_.push_back(s);
            }
            snapshot_gauge_locked(pm);  // single sample at the final state
            cv_.notify_one();
        }
        flush_metrics(pm);  // emit after releasing mu_
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
    std::condition_variable cv_;                            // signals idle availability to acquirers
    std::condition_variable maint_cv_;                      // wakes the reaper to stop
    std::vector<std::unique_ptr<detail::PoolSlot>> slots_;  // all (idle + active)
    std::deque<detail::PoolSlot*> idle_;
    std::size_t pending_ = 0;  // in-flight connects (reserved against max)
    bool stopping_ = false;
    std::thread maintenance_;

    // Observability (resolved in the ctor; raw pointers never null, flags gate
    // emission so the default path costs one branch).
    obs::MetricsSink* metrics_ = nullptr;
    obs::Tracer* tracer_ = nullptr;
    bool has_metrics_ = false;
    bool has_tracer_ = false;
    obs::ILogger* logger_ = nullptr;  // nullable; shared_ptr in config_ keeps it alive

    // --- stats counters (v1.2). Guarded by mu_ EXCEPT createdTotal_, which is
    // atomic because make_connection runs with mu_ deliberately dropped.
    std::size_t waiters_ = 0;
    std::size_t peakBusy_ = 0;
    std::atomic<std::uint64_t> createdTotal_{0};
    std::uint64_t acquiredTotal_ = 0;
    std::uint64_t acquireTimeoutsTotal_ = 0;
    std::uint64_t reapedIdleTotal_ = 0;
    std::uint64_t reapedLifetimeTotal_ = 0;
    std::uint64_t discardedTotal_ = 0;
};

inline void PooledConnection::release() {
    if (pool_ && slot_) {
        pool_->release_slot(slot_, broken_);
        pool_ = nullptr;
        slot_ = nullptr;
    }
}

inline void ConnectionPool::maintain() {
    PendingMetrics pm;
    std::unique_lock<std::mutex> lk(mu_);
    const auto now = config_.now();

    std::deque<detail::PoolSlot*> keep;
    for (detail::PoolSlot* s : idle_) {
        const bool expired_life = (now - s->created_at) >= config_.maxLifetime;
        const bool expired_idle = (now - s->last_used_at) >= config_.idleTimeout;
        const bool over_min = slots_.size() > config_.min;
        if (expired_life || (expired_idle && over_min)) {
            if (expired_life)
                ++reapedLifetimeTotal_;
            else
                ++reapedIdleTotal_;
            remove_slot_locked(s);  // Connection dtor disconnects
            ++pm.reaped;
        } else {
            keep.push_back(s);
        }
    }
    idle_ = std::move(keep);

    // Refill up to min (best effort; a failed connect is retried next pass).
    // Connect WITHOUT mu_ held (reserving via pending_, like acquire()) so a slow
    // Db2 reconnect / backoff sleep on this background path can't block
    // acquire/release/destruction.
    while (!stopping_ && slots_.size() + pending_ < config_.min) {
        ++pending_;
        lk.unlock();
        auto c = make_connection();
        lk.lock();
        --pending_;
        const bool added = c.ok() && !stopping_;
        if (added) add_idle_slot(std::move(c.value()));
        // pending_ was just released (and a slot may have been added), so capacity
        // is free — wake a waiter that blocked on slots_ + pending_ >= max. Doing
        // this every iteration (not only on success) ensures a refill connect
        // failure doesn't leave a waiter asleep until acquireTimeout.
        cv_.notify_one();
        if (!added) break;  // connect failed or shutting down
    }
    // Single gauge sample at the final consistent state (idle_ rebuilt, refill
    // done) — never mid-reap, so active can't read negative.
    snapshot_gauge_locked(pm);
    lk.unlock();
    flush_metrics(pm);  // emit after releasing mu_
}

}  // namespace halcyon
