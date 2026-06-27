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
        : conn_(o.conn_), active_(o.active_), poisoned_(o.poisoned_) {
        o.conn_ = nullptr;
        o.active_ = false;
        o.poisoned_ = false;
    }
    Transaction& operator=(Transaction&& o) noexcept {
        if (this != &o) {
            finish_rollback();
            conn_ = o.conn_;
            active_ = o.active_;
            poisoned_ = o.poisoned_;
            o.conn_ = nullptr;
            o.active_ = false;
            o.poisoned_ = false;
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
        if (!c.ok() || !a.ok()) poisoned_ = true;  // dead conn or autocommit lost
        if (!c.ok()) return c;
        return a;
    }
    Result<void> rollback() {
        if (!active_) return Result<void>();
        auto r = conn_->driver().rollback(conn_->handle());
        active_ = false;
        auto a = conn_->driver().setAutoCommit(conn_->handle(), true);
        if (!r.ok() || !a.ok()) poisoned_ = true;  // dead conn or autocommit lost
        if (!r.ok()) return r;
        return a;
    }

private:
    friend class Connection;
    explicit Transaction(Connection& conn) : conn_(&conn), active_(true) {}

    void finish_rollback() noexcept {
        if (conn_ && active_) {
            auto r = conn_->driver().rollback(conn_->handle());
            auto a = conn_->driver().setAutoCommit(conn_->handle(), true);
            if (!r.ok() || !a.ok()) poisoned_ = true;  // see poisoned()
            active_ = false;
        }
    }

    Connection* conn_;
    bool active_;
    bool poisoned_ = false;
};

inline Result<Transaction> Connection::begin() {
    auto a = driver_->setAutoCommit(handle_, false);
    if (!a.ok()) return a.error();
    return Transaction(*this);
}

}  // namespace halcyon
