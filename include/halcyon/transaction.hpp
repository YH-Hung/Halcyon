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
    Transaction(Transaction&& o) noexcept : conn_(o.conn_), active_(o.active_) {
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

inline Result<Transaction> Connection::begin() {
    auto a = driver_->setAutoCommit(handle_, false);
    if (!a.ok()) return a.error();
    return Transaction(*this);
}

}  // namespace halcyon
