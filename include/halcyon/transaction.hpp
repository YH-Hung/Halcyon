#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "halcyon/connection.hpp"
#include "halcyon/error.hpp"
#include "halcyon/isolation.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/result.hpp"

namespace halcyon {

namespace detail {

// Savepoint statements cannot be parameterized, so the name is interpolated
// into SQL. This whitelist is the injection defense — a hard gate, not a
// convention: [A-Za-z_][A-Za-z0-9_]{0,127}, not starting with SYS (Db2
// reserves that prefix).
inline bool valid_savepoint_name(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    const auto first = static_cast<unsigned char>(name[0]);
    if (!std::isalpha(first) && name[0] != '_') return false;
    for (char ch : name.substr(1)) {
        const auto c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '_') return false;
    }
    if (name.size() >= 3) {
        auto up = [](char c) {
            return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        };
        if (up(name[0]) == 'S' && up(name[1]) == 'Y' && up(name[2]) == 'S')
            return false;
    }
    return true;
}

}  // namespace detail

class Savepoint;  // defined after Transaction

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
          restoreIsolation_(std::move(o.restoreIsolation_)),
          spCounter_(o.spCounter_) {
        o.conn_ = nullptr;
        o.active_ = false;
        o.poisoned_ = false;
        o.restoreIsolation_.reset();
        o.spCounter_ = 0;
    }
    Transaction& operator=(Transaction&& o) noexcept {
        if (this != &o) {
            finish_rollback();
            conn_ = o.conn_;
            active_ = o.active_;
            poisoned_ = o.poisoned_;
            restoreIsolation_ = std::move(o.restoreIsolation_);
            spCounter_ = o.spCounter_;
            o.conn_ = nullptr;
            o.active_ = false;
            o.poisoned_ = false;
            o.restoreIsolation_.reset();
            o.spCounter_ = 0;
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

    // Streaming query within the unit of work; defined in streaming.hpp.
    template <class... Args>
    Result<StreamingResultSet> queryStreaming(const std::string& sql,
                                              const Args&... args);
    Result<StreamingResultSet> queryStreaming(const std::string& sql,
                                              const params& named);

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

    // Creates a savepoint with an auto-generated per-transaction name
    // (halcyon_sp_1, halcyon_sp_2, ...). See class Savepoint for semantics.
    Result<Savepoint> savepoint();
    // Creates a savepoint with an explicit name. The name must match
    // [A-Za-z_][A-Za-z0-9_]{0,127} and must not begin with SYS
    // (ErrorCode::InvalidArgument otherwise).
    Result<Savepoint> savepoint(const std::string& name);

    // Runs fn inside a savepoint scope on THIS transaction: an ok Result
    // releases the savepoint, an error Result or exception rolls back to it
    // (the error/exception propagates). Calling commit()/rollback() on the
    // transaction inside fn is a programming error -> ErrorCode::InvalidState.
    // fn must return Result<U>.
    template <class Fn>
    auto nested(Fn&& fn) -> std::invoke_result_t<Fn, Transaction&>;

private:
    friend class Connection;
    friend class Savepoint;
    explicit Transaction(Connection& conn) : conn_(&conn), active_(true) {}

    Result<Savepoint> make_savepoint(std::string name);

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
        if (!r.ok() && conn_ != nullptr && conn_->logger() != nullptr)
            conn_->logger()->log(obs::LogLevel::Error, "isolation.restore_fail",
                                 {{"code", to_string(r.error().code)}});
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
    int spCounter_ = 0;  // auto savepoint names; moved with the transaction
};

/// \brief One-shot RAII guard over a SQL savepoint inside a Transaction.
///
/// `rollback()` emits ROLLBACK TO SAVEPOINT; `release()` emits RELEASE
/// SAVEPOINT; if neither is called the destructor rolls back to the savepoint
/// and releases it (undo by default, mirroring Transaction). A failed
/// terminal statement poisons this savepoint AND the owning transaction.
/// Move-only. Must be destroyed before the owning Transaction ends or moves;
/// destroy inner savepoints before outer ones.
class Savepoint {
public:
    Savepoint(Savepoint&& o) noexcept
        : tx_(o.tx_), name_(std::move(o.name_)), active_(o.active_),
          poisoned_(o.poisoned_) {
        o.tx_ = nullptr;
        o.active_ = false;
        o.poisoned_ = false;
    }
    Savepoint& operator=(Savepoint&& o) noexcept {
        if (this != &o) {
            finish_rollback();
            tx_ = o.tx_;
            name_ = std::move(o.name_);
            active_ = o.active_;
            poisoned_ = o.poisoned_;
            o.tx_ = nullptr;
            o.active_ = false;
            o.poisoned_ = false;
        }
        return *this;
    }
    Savepoint(const Savepoint&) = delete;
    Savepoint& operator=(const Savepoint&) = delete;
    ~Savepoint() { finish_rollback(); }

    const std::string& name() const noexcept { return name_; }
    bool active() const noexcept { return active_; }
    bool poisoned() const noexcept { return poisoned_; }

    // Undoes all work after the savepoint (the savepoint itself survives in
    // Db2 and is released when the transaction ends). One-shot.
    Result<void> rollback() {
        if (tx_ == nullptr || !active_) return Result<void>();
        active_ = false;
        auto r = tx_->execute("ROLLBACK TO SAVEPOINT " + name_);
        if (!r.ok()) {
            poison();
            return r.error();
        }
        return Result<void>();
    }

    // Keeps the work and frees the savepoint. One-shot.
    Result<void> release() {
        if (tx_ == nullptr || !active_) return Result<void>();
        active_ = false;
        auto r = tx_->execute("RELEASE SAVEPOINT " + name_);
        if (!r.ok()) {
            poison();
            return r.error();
        }
        return Result<void>();
    }

private:
    friend class Transaction;
    Savepoint(Transaction* tx, std::string name)
        : tx_(tx), name_(std::move(name)), active_(true) {}

    void poison() noexcept {
        poisoned_ = true;
        if (auto* lg = tx_->conn_->logger())
            lg->log(obs::LogLevel::Error, "savepoint.poisoned",
                    {{"name", name_}});
        tx_->poison();
    }

    void finish_rollback() noexcept {
        if (tx_ != nullptr && active_) {
            active_ = false;
            auto r = tx_->execute("ROLLBACK TO SAVEPOINT " + name_);
            auto rel = tx_->execute("RELEASE SAVEPOINT " + name_);
            if (!r.ok() || !rel.ok()) poison();
        }
    }

    Transaction* tx_ = nullptr;
    std::string name_;
    bool active_ = false;
    bool poisoned_ = false;
};

inline Result<Savepoint> Transaction::savepoint() {
    return make_savepoint("halcyon_sp_" + std::to_string(++spCounter_));
}

inline Result<Savepoint> Transaction::savepoint(const std::string& name) {
    if (!detail::valid_savepoint_name(name)) {
        Error e;
        e.code = ErrorCode::InvalidArgument;
        e.message = "invalid savepoint name '" + name +
                    "': must match [A-Za-z_][A-Za-z0-9_]{0,127} and not begin "
                    "with SYS";
        return e;
    }
    return make_savepoint(name);
}

inline Result<Savepoint> Transaction::make_savepoint(std::string name) {
    if (!active_) {
        Error e;
        e.code = ErrorCode::InvalidState;
        e.message = "savepoint requires an active transaction";
        return e;
    }
    auto r = execute("SAVEPOINT " + name + " ON ROLLBACK RETAIN CURSORS");
    if (!r.ok()) return r.error();
    return Savepoint(this, std::move(name));
}

template <class Fn>
auto Transaction::nested(Fn&& fn) -> std::invoke_result_t<Fn, Transaction&> {
    using R = std::invoke_result_t<Fn, Transaction&>;
    auto sp = savepoint();
    if (!sp.ok()) return R(sp.error());
    R r = [&]() -> R {
        try {
            return std::forward<Fn>(fn)(*this);
        } catch (...) {
            sp.value().rollback();
            throw;
        }
    }();
    if (!active_) {
        // fn ended the transaction (commit/rollback inside nested()): the
        // savepoint no longer exists, so disarm the guard. Otherwise its
        // destructor would run ROLLBACK TO / RELEASE on an ended transaction,
        // fail, and needlessly poison and discard the pooled connection.
        sp.value().active_ = false;
        Error e;
        e.code = ErrorCode::InvalidState;
        e.message = "transaction ended inside nested scope "
                    "(commit/rollback inside nested() is not allowed)";
        return R(e);
    }
    if (r.ok()) {
        auto rel = sp.value().release();
        if (!rel.ok()) return R(rel.error());
    } else {
        sp.value().rollback();
    }
    return r;
}

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
