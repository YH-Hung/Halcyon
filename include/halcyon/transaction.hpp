#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "halcyon/connection.hpp"
#include "halcyon/error.hpp"
#include "halcyon/isolation.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/result.hpp"

namespace halcyon {

/// \brief RAII unit of work over a single `Connection`.
///
/// Autocommit is disabled for the transaction's lifetime. Call `commit()` or
/// `rollback()` to end it; if neither is called the destructor rolls back.
/// Move-only; never auto-retried — the caller must re-drive the whole
/// transaction on failure.
class Transaction {
public:
    Transaction(Transaction&& o) noexcept
        : conn_(o.conn_),
          active_(o.active_),
          poisoned_(o.poisoned_),
          restoreIsolation_(std::move(o.restoreIsolation_)) {
        o.conn_ = nullptr;
        o.active_ = false;
        o.poisoned_ = false;
        o.restoreIsolation_.reset();
    }
    Transaction& operator=(Transaction&& o) noexcept {
        if (this != &o) {
            finish_rollback();
            conn_ = o.conn_;
            active_ = o.active_;
            poisoned_ = o.poisoned_;
            restoreIsolation_ = std::move(o.restoreIsolation_);
            o.conn_ = nullptr;
            o.active_ = false;
            o.poisoned_ = false;
            o.restoreIsolation_.reset();
        }
        return *this;
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction() { finish_rollback(); }

    bool active() const noexcept { return active_; }

    // True if a commit/rollback (or the implicit rollback on destruction) failed,
    // leaving the connection dead or with autocommit in an unknown state. The
    // owner (ScopedTransaction) discards such a connection rather than returning
    // it to the pool for reuse.
    bool poisoned() const noexcept { return poisoned_; }

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

    // Batch insert within the unit of work; forwards to the leased connection.
    Result<std::int64_t> executeBatch(
        const std::string& sql,
        const std::vector<std::vector<detail::cli::Value>>& rows) {
        return conn_->executeBatch(sql, rows);
    }
    // Ergonomic Batch overload so `tx.executeBatch(sql, batchOf(rows))` works in
    // the functional transaction facade, matching ScopedTransaction.
    Result<std::int64_t> executeBatch(const std::string& sql,
                                      const Batch& batch) {
        return conn_->executeBatch(sql, batch.rows);
    }

    Result<void> commit() {
        if (!active_) return Result<void>();
        auto c = conn_->driver().commit(conn_->handle());
        active_ = false;
        auto a = conn_->driver().setAutoCommit(conn_->handle(), true);
        auto i = restore_isolation();
        if (!c.ok() || !a.ok() || !i.ok()) poison();  // dead conn or session state lost
        if (!c.ok()) return c;
        if (!a.ok()) return a;
        return i;
    }
    Result<void> rollback() {
        if (!active_) return Result<void>();
        auto r = conn_->driver().rollback(conn_->handle());
        active_ = false;
        auto a = conn_->driver().setAutoCommit(conn_->handle(), true);
        auto i = restore_isolation();
        if (!r.ok() || !a.ok() || !i.ok()) poison();  // dead conn or session state lost
        if (!r.ok()) return r;
        if (!a.ok()) return a;
        return i;
    }

private:
    friend class Connection;
    explicit Transaction(Connection& conn) : conn_(&conn), active_(true) {}

    // Central poison point: marks the transaction unusable and logs once.
    void poison() noexcept {
        if (!poisoned_ && conn_ != nullptr && conn_->logger() != nullptr)
            conn_->logger()->log(obs::LogLevel::Error, "txn.poisoned", {});
        poisoned_ = true;
    }

    // Restores the pre-override isolation level, if this transaction set one.
    Result<void> restore_isolation() {
        if (!restoreIsolation_) return Result<void>();
        auto r = conn_->driver().setIsolation(conn_->handle(), *restoreIsolation_);
        restoreIsolation_.reset();
        return r;
    }

    void finish_rollback() noexcept {
        if (conn_ && active_) {
            auto r = conn_->driver().rollback(conn_->handle());
            auto a = conn_->driver().setAutoCommit(conn_->handle(), true);
            auto i = restore_isolation();
            if (!r.ok() || !a.ok() || !i.ok()) poison();  // see poisoned()
            active_ = false;
        }
    }

    Connection* conn_;
    bool active_;
    bool poisoned_ = false;
    std::optional<Isolation> restoreIsolation_;
};

inline Result<Transaction> Connection::begin() {
    auto a = driver_->setAutoCommit(handle_, false);
    if (!a.ok()) return a.error();
    return Transaction(*this);
}

inline Result<Transaction> Connection::begin(Isolation level) {
    auto s = driver_->setIsolation(handle_, level);
    if (!s.ok()) return s.error();
    auto a = driver_->setAutoCommit(handle_, false);
    if (!a.ok()) {
        // Best-effort: don't leave the override behind on the failure path.
        driver_->setIsolation(
            handle_, defaultIsolation_.value_or(Isolation::CursorStability));
        return a.error();
    }
    Transaction t(*this);
    t.restoreIsolation_ =
        defaultIsolation_.value_or(Isolation::CursorStability);
    return t;
}

}  // namespace halcyon
