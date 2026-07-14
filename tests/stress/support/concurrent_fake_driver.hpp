#pragma once

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/isolation.hpp"

namespace halcyon::stress {

// A thread-safe ICliDriver for concurrency testing. Unlike MockCliDriver's FIFO
// scripting, every response is derived from its inputs, so any number of threads
// may call it with no ordering assumptions. Counters are atomic; the handle maps
// are guarded by one mutex (the fake is intentionally a little pessimistic — the
// real contention we measure lives in Halcyon's pool/executor, not here).
class ConcurrentFakeDriver final : public detail::cli::ICliDriver {
public:
    using ConnectionHandle = detail::cli::ConnectionHandle;
    using StatementHandle = detail::cli::StatementHandle;
    using ConnectionParams = detail::cli::ConnectionParams;
    using Value = detail::cli::Value;

    // --- realism knobs (microseconds; 0 = no sleep) ---
    std::atomic<long> queryLatencyUs{0};
    std::atomic<long> connectLatencyUs{0};

    // --- fault injection (1-in-N; 0 disables) ---
    std::atomic<int> failConnectEvery{0};
    std::atomic<int> failExecuteEvery{0};
    std::atomic<int> killConnectionEvery{0};

    // --- counters ---
    std::atomic<long> connectCalls{0};
    std::atomic<long> disconnectCalls{0};
    std::atomic<long> prepareCalls{0};
    std::atomic<long> executeCalls{0};
    std::atomic<long> closeCursorCalls{0};
    std::atomic<long> aliveCalls{0};
    std::atomic<long> finalizeCalls{0};
    std::atomic<long> commitCalls{0};
    std::atomic<long> rollbackCalls{0};
    std::atomic<long> autoCommitOff{0};
    std::atomic<long> autoCommitOn{0};
    std::atomic<long> setIsolationCalls{0};
    std::atomic<long> inFlight{0};
    std::atomic<long> peakInFlight{0};

    // --- Connection lifecycle ---
    Result<ConnectionHandle> connect(const ConnectionParams&) override {
        const long n = ++connectCalls;
        sleep_us(connectLatencyUs.load());
        const int every = failConnectEvery.load();
        if (every > 0 && n % every == 0) return transient<ConnectionHandle>();
        const auto h = static_cast<ConnectionHandle>(next_conn_.fetch_add(1) + 1);
        std::lock_guard<std::mutex> lk(mu_);
        live_.insert(h);
        connStates_[h] = ConnState{};  // Db2 defaults: autocommit on, isolation unset
        return Result<ConnectionHandle>(h);
    }

    Result<void> disconnect(ConnectionHandle h) override {
        ++disconnectCalls;
        std::lock_guard<std::mutex> lk(mu_);
        live_.erase(h);
        dead_.erase(h);
        return Result<void>();
    }

    Result<bool> isAlive(ConnectionHandle h) override {
        ++aliveCalls;
        std::lock_guard<std::mutex> lk(mu_);
        if (dead_.count(h)) return Result<bool>(false);
        return Result<bool>(live_.count(h) != 0);
    }

    // --- Prepared-statement path ---
    Result<StatementHandle> prepare(ConnectionHandle conn,
                                    const std::string& sql) override {
        ++prepareCalls;
        std::lock_guard<std::mutex> lk(mu_);
        if (!live_.count(conn)) return stale<StatementHandle>();
        const auto h = static_cast<StatementHandle>(next_stmt_.fetch_add(1) + 1);
        stmts_[h] = StmtState{conn, sql, -1};
        return Result<StatementHandle>(h);
    }

    Result<void> bindParams(StatementHandle stmt,
                            const std::vector<Value>&) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = stmts_.find(stmt);
        if (it == stmts_.end() || !live_.count(it->second.conn))
            return stale<void>();
        return Result<void>();
    }

    Result<std::int64_t> execute(StatementHandle stmt) override {
        const long n = ++executeCalls;
        enter_in_flight();
        sleep_us(queryLatencyUs.load());
        InFlightGuard g{this};

        const int fe = failExecuteEvery.load();
        if (fe > 0 && n % fe == 0) return transient<std::int64_t>();

        std::lock_guard<std::mutex> lk(mu_);
        auto it = stmts_.find(stmt);
        if (it == stmts_.end() || !live_.count(it->second.conn))
            return stale<std::int64_t>();
        it->second.position = -1;
        const int ke = killConnectionEvery.load();
        if (ke > 0 && n % ke == 0) {
            live_.erase(it->second.conn);  // next isAlive -> false
            dead_.insert(it->second.conn);
        }
        return Result<std::int64_t>(is_select(it->second.sql) ? 0 : 1);
    }

    // Array binding (executeBatch seam). Derived from the same per-row logic as
    // execute(), so fault injection and counters behave identically; returns the
    // summed affected-row count.
    Result<std::int64_t> executeBatch(
        StatementHandle stmt,
        const std::vector<std::vector<Value>>& rows) override {
        std::int64_t total = 0;
        for (const auto& row : rows) {
            auto b = bindParams(stmt, row);
            if (!b.ok()) return b.error();
            auto e = execute(stmt);
            if (!e.ok()) return e.error();
            total += e.value();
        }
        return total;
    }

    Result<std::size_t> columnCount(StatementHandle stmt) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = stmts_.find(stmt);
        if (it == stmts_.end()) return Result<std::size_t>(std::size_t{0});
        return Result<std::size_t>(is_select(it->second.sql) ? std::size_t{1}
                                                             : std::size_t{0});
    }

    Result<std::string> columnName(StatementHandle, std::size_t) override {
        return Result<std::string>(std::string{"c0"});
    }

    Result<std::vector<std::vector<Value>>> fetchBlock(
        StatementHandle stmt, std::size_t maxRows) override {
        (void)maxRows;
        std::lock_guard<std::mutex> lk(mu_);
        auto it = stmts_.find(stmt);
        if (it == stmts_.end()) return mapping<std::vector<std::vector<Value>>>();
        std::vector<std::vector<Value>> out;
        if (it->second.position < 0) {  // single row, once
            it->second.position = 0;
            out.push_back({Value{encoded_value(it->second.sql)}});
        }
        return Result<std::vector<std::vector<Value>>>(std::move(out));
    }

    Result<void> finalize(StatementHandle stmt) override {
        ++finalizeCalls;
        std::lock_guard<std::mutex> lk(mu_);
        stmts_.erase(stmt);
        return Result<void>();
    }

    Result<void> closeCursor(StatementHandle stmt) override {
        ++closeCursorCalls;
        std::lock_guard<std::mutex> lk(mu_);
        auto it = stmts_.find(stmt);
        if (it != stmts_.end()) it->second.position = -1;
        return Result<void>();
    }

    // --- Transaction control ---
    Result<void> setAutoCommit(ConnectionHandle conn, bool enabled) override {
        if (enabled) ++autoCommitOn;
        else ++autoCommitOff;
        std::lock_guard<std::mutex> lk(mu_);
        connStates_[conn].autocommit = enabled;
        return Result<void>();
    }
    Result<void> setIsolation(ConnectionHandle conn, Isolation level) override {
        setIsolationCalls.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mu_);
        connStates_[conn].isolation = level;
        return Result<void>();
    }

    // Per-connection session state so a test can assert every surviving
    // connection ended in a restored state, not just that global call counts
    // balance (which cannot detect a wrong value or offsetting per-connection
    // errors).
    struct ConnState {
        bool autocommit = true;  // Db2 default: autocommit on
        std::optional<Isolation> isolation;  // unset = server default (CS)
    };
    std::vector<ConnState> connection_states() {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<ConnState> out;
        out.reserve(connStates_.size());
        for (const auto& kv : connStates_) out.push_back(kv.second);
        return out;
    }

    // --- Streaming data path (v1.1): not exercised by the stress suite ---
    Result<bool> fetchNext(StatementHandle) override {
        return Result<bool>(false);
    }
    Result<Value> getValue(StatementHandle, std::size_t) override {
        return Result<Value>(unsupported("getValue"));
    }
    Result<detail::cli::GetDataChunk> getDataChunk(StatementHandle, std::size_t,
                                                   std::byte*,
                                                   std::size_t) override {
        return Result<detail::cli::GetDataChunk>(unsupported("getDataChunk"));
    }
    Result<std::int64_t> executeStreaming(
        StatementHandle, const std::vector<Value>&,
        std::vector<detail::cli::ParamStreamSource>) override {
        return Result<std::int64_t>(unsupported("executeStreaming"));
    }
    Result<void> commit(ConnectionHandle) override {
        ++commitCalls;
        return Result<void>();
    }
    Result<void> rollback(ConnectionHandle) override {
        ++rollbackCalls;
        return Result<void>();
    }

private:
    struct StmtState {
        ConnectionHandle conn;
        std::string sql;
        long position;
    };
    struct InFlightGuard {
        ConcurrentFakeDriver* d;
        ~InFlightGuard() { --d->inFlight; }
    };

    void enter_in_flight() {
        const long cur = ++inFlight;
        long prev = peakInFlight.load();
        while (cur > prev && !peakInFlight.compare_exchange_weak(prev, cur)) {}
    }

    static void sleep_us(long us) {
        if (us > 0) std::this_thread::sleep_for(std::chrono::microseconds(us));
    }

    // Leading non-space token is SELECT or VALUES (the fake never sees real DML
    // result sets; DML returns an affected-row count instead).
    static Error unsupported(const char* what) {
        Error e;
        e.code = ErrorCode::Unknown;
        e.message = std::string(what) + " unsupported in ConcurrentFakeDriver";
        return e;
    }

    static bool is_select(const std::string& sql) {
        std::size_t i = 0;
        while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i])))
            ++i;
        auto starts = [&](const char* kw) {
            std::size_t k = 0;
            while (kw[k] && i + k < sql.size() &&
                   std::toupper(static_cast<unsigned char>(sql[i + k])) == kw[k])
                ++k;
            return kw[k] == '\0';
        };
        return starts("SELECT") || starts("VALUES");
    }

    // The first contiguous run of digits in the SQL (e.g. "SELECT 7 FROM ..." ->
    // 7). Both fake and live backends run "SELECT <n> FROM SYSIBM.SYSDUMMY1", so a
    // workload always knows the expected scalar. No digits -> 0.
    static std::int64_t encoded_value(const std::string& sql) {
        std::int64_t v = 0;
        bool in = false;
        for (char ch : sql) {
            if (ch >= '0' && ch <= '9') {
                v = v * 10 + (ch - '0');
                in = true;
            } else if (in) {
                break;
            }
        }
        return v;
    }

    template <class T>
    static Result<T> transient() {
        Error e;
        e.code = ErrorCode::Transient;
        e.sqlstate = "40001";
        e.message = "injected transient";
        e.retriable = true;
        return Result<T>(e);
    }
    template <class T>
    static Result<T> stale() {
        Error e;
        e.code = ErrorCode::Connection;
        e.sqlstate = "08003";
        e.message = "statement used on a dead connection";
        e.retriable = true;
        return Result<T>(e);
    }
    template <class T>
    static Result<T> mapping() {
        Error e;
        e.code = ErrorCode::Mapping;
        e.message = "column index out of range";
        return Result<T>(e);
    }

    std::mutex mu_;
    std::unordered_set<ConnectionHandle> live_;
    std::unordered_set<ConnectionHandle> dead_;
    std::unordered_map<StatementHandle, StmtState> stmts_;
    std::unordered_map<ConnectionHandle, ConnState> connStates_;
    std::atomic<std::uint64_t> next_conn_{0};
    std::atomic<std::uint64_t> next_stmt_{0};
};

}  // namespace halcyon::stress
