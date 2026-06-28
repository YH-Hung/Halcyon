#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"

namespace halcyon::testing {

// A scriptable fake driver: queue connect/prepare/execute outcomes and result
// grids so the entire core (binding, mapping, iteration) can be tested with no
// live Db2.
class MockCliDriver final : public detail::cli::ICliDriver {
public:
    using ConnectionHandle = detail::cli::ConnectionHandle;
    using StatementHandle = detail::cli::StatementHandle;
    using ConnectionParams = detail::cli::ConnectionParams;
    using Value = detail::cli::Value;

    struct ScriptedRows {
        std::vector<std::string> columns;
        std::vector<std::vector<Value>> rows;
    };

    struct StmtState {
        std::string sql;
        std::vector<Value> boundParams;
        ScriptedRows cursor;
        long position = -1;  // -1 before first fetch
    };

    // --- connect scripting (Plan 1) ---
    std::deque<Error> connectErrors;
    std::deque<bool> aliveResults;
    int connectCalls = 0;
    int disconnectCalls = 0;
    int aliveCalls = 0;
    std::vector<ConnectionParams> connectParams;

    // --- statement scripting (Plan 2) ---
    std::deque<Error> prepareErrors;
    std::deque<Error> executeErrors;
    std::deque<ScriptedRows> resultSets;     // execute() attaches the next one
    std::deque<std::int64_t> execRowCounts;  // execute() returns the next one

    // --- batch scripting (array binding) ---
    std::deque<std::int64_t> batchRowCounts;  // executeBatch() returns the next
    std::deque<Error> batchErrors;            // executeBatch() fails with the next
    int executeBatchCalls = 0;
    std::vector<std::vector<Value>> lastBatchRows;  // rows from the last call

    // Optional gate invoked at the top of every execute(); lets a test block a
    // worker thread mid-call to make async/lifetime ordering deterministic.
    std::function<void()> executeHook;

    // Optional gate invoked at the top of every connect(); lets a test block a
    // worker thread mid-connect to assert the pool mutex is not held meanwhile.
    std::function<void()> connectHook;

    std::vector<std::string> preparedSql;
    std::map<StatementHandle, StmtState> statements;

    Result<ConnectionHandle> connect(const ConnectionParams& params) override {
        if (connectHook) connectHook();
        ++connectCalls;
        connectParams.push_back(params);
        if (!connectErrors.empty()) {
            Error e = connectErrors.front();
            connectErrors.pop_front();
            return Result<ConnectionHandle>(e);
        }
        return Result<ConnectionHandle>(
            static_cast<ConnectionHandle>(++nextConn_));
    }

    Result<void> disconnect(ConnectionHandle handle) override {
        ++disconnectCalls;
        (void)handle;
        return Result<void>();
    }

    Result<bool> isAlive(ConnectionHandle handle) override {
        ++aliveCalls;
        (void)handle;
        if (!aliveResults.empty()) {
            bool v = aliveResults.front();
            aliveResults.pop_front();
            return Result<bool>(v);
        }
        return Result<bool>(true);
    }

    Result<StatementHandle> prepare(ConnectionHandle conn,
                                    const std::string& sql) override {
        (void)conn;
        if (!prepareErrors.empty()) {
            Error e = prepareErrors.front();
            prepareErrors.pop_front();
            return Result<StatementHandle>(e);
        }
        preparedSql.push_back(sql);
        auto h = static_cast<StatementHandle>(++nextStmt_);
        statements[h] = StmtState{sql, {}, {}, -1};
        return Result<StatementHandle>(h);
    }

    Result<void> bindParams(StatementHandle stmt,
                            const std::vector<Value>& params) override {
        statements.at(stmt).boundParams = params;
        return Result<void>();
    }

    Result<std::int64_t> executeBatch(
        StatementHandle stmt,
        const std::vector<std::vector<Value>>& rows) override {
        (void)stmt;
        ++executeBatchCalls;
        lastBatchRows = rows;
        if (!batchErrors.empty()) {
            Error e = batchErrors.front();
            batchErrors.pop_front();
            return Result<std::int64_t>(e);
        }
        if (!batchRowCounts.empty()) {
            std::int64_t n = batchRowCounts.front();
            batchRowCounts.pop_front();
            return Result<std::int64_t>(n);
        }
        return Result<std::int64_t>(static_cast<std::int64_t>(rows.size()));
    }

    Result<std::int64_t> execute(StatementHandle stmt) override {
        if (executeHook) executeHook();
        if (!executeErrors.empty()) {
            Error e = executeErrors.front();
            executeErrors.pop_front();
            return Result<std::int64_t>(e);
        }
        auto& s = statements.at(stmt);
        s.position = -1;
        if (!resultSets.empty()) {
            s.cursor = resultSets.front();
            resultSets.pop_front();
            return Result<std::int64_t>(std::int64_t{0});
        }
        s.cursor = ScriptedRows{};
        if (!execRowCounts.empty()) {
            std::int64_t n = execRowCounts.front();
            execRowCounts.pop_front();
            return Result<std::int64_t>(n);
        }
        return Result<std::int64_t>(std::int64_t{0});
    }

    Result<std::size_t> columnCount(StatementHandle stmt) override {
        return Result<std::size_t>(statements.at(stmt).cursor.columns.size());
    }

    Result<std::string> columnName(StatementHandle stmt,
                                   std::size_t index) override {
        const auto& cols = statements.at(stmt).cursor.columns;
        if (index >= cols.size()) return Result<std::string>(rangeError());
        return Result<std::string>(cols[index]);
    }

    // --- block fetch scripting ---
    std::size_t fetchBlockSize = 0;  // 0 => up to maxRows; >0 => cap per call
    Error fetchBlockError;
    int failFetchBlockOnCall = 0;  // 1-based fetchBlock() call to fail; 0=never
    int fetchBlockCalls = 0;

    Result<std::vector<std::vector<Value>>> fetchBlock(
        StatementHandle stmt, std::size_t maxRows) override {
        ++fetchBlockCalls;
        if (failFetchBlockOnCall != 0 && fetchBlockCalls == failFetchBlockOnCall)
            return Result<std::vector<std::vector<Value>>>(fetchBlockError);
        auto& s = statements.at(stmt);
        std::size_t cap = (fetchBlockSize != 0)
                              ? std::min(maxRows, fetchBlockSize)
                              : maxRows;
        std::vector<std::vector<Value>> out;
        const long n = static_cast<long>(s.cursor.rows.size());
        while (out.size() < cap && s.position + 1 < n) {
            ++s.position;
            out.push_back(s.cursor.rows[static_cast<std::size_t>(s.position)]);
        }
        return Result<std::vector<std::vector<Value>>>(std::move(out));
    }

    Result<void> finalize(StatementHandle stmt) override {
        // Retain the recorded StmtState (bound params, last cursor) so tests can
        // still introspect a statement after the owning Statement/ResultSet has
        // been destroyed (handles are never reused, so this can't alias). Record
        // the finalize for assertions that care.
        ++finalizeCalls;
        finalized.push_back(stmt);
        return Result<void>();
    }

    Result<void> closeCursor(StatementHandle stmt) override {
        ++closeCursorCalls;
        auto it = statements.find(stmt);
        if (it != statements.end()) it->second.position = -1;  // cursor reset
        if (!closeCursorErrors.empty()) {
            Error e = closeCursorErrors.front();
            closeCursorErrors.pop_front();
            return Result<void>(e);
        }
        return Result<void>();
    }

    int finalizeCalls = 0;
    std::vector<StatementHandle> finalized;
    int closeCursorCalls = 0;
    std::deque<Error> closeCursorErrors;  // next closeCursor() fails with each

    // --- transaction scripting (Plan 4) ---
    std::deque<Error> txnErrors;  // next setAutoCommit/commit/rollback fails
    std::vector<bool> autoCommitCalls;
    int commitCalls = 0;
    int rollbackCalls = 0;

    Result<void> setAutoCommit(ConnectionHandle handle, bool enabled) override {
        (void)handle;
        autoCommitCalls.push_back(enabled);
        return next_txn_result();
    }
    Result<void> commit(ConnectionHandle handle) override {
        (void)handle;
        ++commitCalls;
        return next_txn_result();
    }
    Result<void> rollback(ConnectionHandle handle) override {
        (void)handle;
        ++rollbackCalls;
        return next_txn_result();
    }

private:
    Result<void> next_txn_result() {
        if (!txnErrors.empty()) {
            Error e = txnErrors.front();
            txnErrors.pop_front();
            return Result<void>(e);
        }
        return Result<void>();
    }

    static Error rangeError() {
        Error e;
        e.code = ErrorCode::Mapping;
        e.message = "column index out of range";
        return e;
    }

    std::uint64_t nextConn_ = 0;
    std::uint64_t nextStmt_ = 0;
};

}  // namespace halcyon::testing
