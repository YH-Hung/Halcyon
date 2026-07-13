#pragma once

#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/observability/logging.hpp"
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
                   std::size_t capacity, obs::MetricsSink* metrics = nullptr,
                   obs::ILogger* logger = nullptr)
        : driver_(&driver),
          conn_(conn),
          capacity_(capacity),
          metrics_(metrics),
          logger_(logger) {}

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
        // Log before erase_entry(e) destroys the entry (and its key string).
        if (logger_ != nullptr)
            logger_->log(obs::LogLevel::Debug, "stmt_cache.evict",
                         {{"sql", e->key}});
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
        // Best-effort cursor reset. If closeCursor fails the handle may be only
        // half-reset; swallow the error but drop the entry so a bad handle is
        // never served to a later acquire (spec §7).
        if (!driver_->closeCursor(e->handle).ok()) {
            driver_->finalize(e->handle);
            erase_entry(e);
            emit_size();
            return;
        }
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
    obs::ILogger* logger_;
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
