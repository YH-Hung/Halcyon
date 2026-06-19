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

}  // namespace halcyon
