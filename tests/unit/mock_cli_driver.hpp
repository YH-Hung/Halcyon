#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/isolation.hpp"

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
        long position = -1;                   // -1 before first fetch
        std::vector<std::size_t> lobOffsets;  // per-column getDataChunk progress
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

    // Lifetime probe: invoked at the top of every disconnect() so a test can
    // count pool teardown without holding its own strong driver reference.
    std::function<void()> disconnectHook;

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
        if (disconnectHook) disconnectHook();
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
        statements[h] = StmtState{sql, {}, {}, -1, {}};
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
        if (onCloseCursor) onCloseCursor();
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
    // Ordering probes: invoked at the top of closeCursor()/rollback() so a test
    // can record when the cursor/transaction is torn down relative to other
    // observable events (e.g. the pool gauge emitted when a lease returns).
    std::function<void()> onCloseCursor;
    std::function<void()> onRollback;

    // Ordering/threading probe: invoked at the top of commit() so a test can
    // record which thread commits (e.g. the coro transaction hop-back).
    std::function<void()> commitHook;

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
        if (commitHook) commitHook();
        (void)handle;
        ++commitCalls;
        return next_txn_result();
    }
    Result<void> rollback(ConnectionHandle handle) override {
        (void)handle;
        ++rollbackCalls;
        if (onRollback) onRollback();
        return next_txn_result();
    }

    // --- isolation scripting (v1.1) ---
    std::vector<std::pair<ConnectionHandle, halcyon::Isolation>> isolationCalls;
    std::deque<Error> isolationErrors;

    Result<void> setIsolation(ConnectionHandle conn,
                              halcyon::Isolation level) override {
        isolationCalls.emplace_back(conn, level);
        if (!isolationErrors.empty()) {
            Error e = isolationErrors.front();
            isolationErrors.pop_front();
            return Result<void>(e);
        }
        return Result<void>();
    }

    // --- streaming scripting (v1.1) ---
    std::deque<Error> fetchNextErrors;
    std::deque<Error> getDataChunkErrors;
    std::deque<Error> executeStreamingErrors;
    std::deque<std::int64_t> streamRowCounts;
    std::size_t lobChunkCap = 0;    // max bytes per getDataChunk; 0 = fill buf
    std::size_t streamPullCap = 8;  // mock's pull buffer, small to force chunking
    int fetchNextCalls = 0;
    int getDataChunkCalls = 0;
    int executeStreamingCalls = 0;
    std::vector<Value> lastStreamValues;
    std::map<std::size_t, std::vector<std::byte>> lastStreamedLobs;

    Result<bool> fetchNext(StatementHandle stmt) override {
        ++fetchNextCalls;
        if (!fetchNextErrors.empty()) {
            Error e = fetchNextErrors.front();
            fetchNextErrors.pop_front();
            return Result<bool>(e);
        }
        auto& s = statements.at(stmt);
        if (s.position + 1 >= static_cast<long>(s.cursor.rows.size()))
            return Result<bool>(false);
        ++s.position;
        s.lobOffsets.assign(s.cursor.columns.size(), 0);
        return Result<bool>(true);
    }

    Result<Value> getValue(StatementHandle stmt, std::size_t col) override {
        auto& s = statements.at(stmt);
        if (s.position < 0 ||
            s.position >= static_cast<long>(s.cursor.rows.size()) ||
            col >= s.cursor.rows[static_cast<std::size_t>(s.position)].size())
            return Result<Value>(rangeError());
        return Result<Value>(
            s.cursor.rows[static_cast<std::size_t>(s.position)][col]);
    }

    Result<detail::cli::GetDataChunk> getDataChunk(StatementHandle stmt,
                                                   std::size_t col,
                                                   std::byte* buf,
                                                   std::size_t bufLen) override {
        ++getDataChunkCalls;
        if (!getDataChunkErrors.empty()) {
            Error e = getDataChunkErrors.front();
            getDataChunkErrors.pop_front();
            return Result<detail::cli::GetDataChunk>(e);
        }
        auto& s = statements.at(stmt);
        if (s.position < 0 ||
            col >= s.cursor.rows[static_cast<std::size_t>(s.position)].size())
            return Result<detail::cli::GetDataChunk>(rangeError());
        const Value& cell =
            s.cursor.rows[static_cast<std::size_t>(s.position)][col];
        detail::cli::GetDataChunk out;
        if (std::holds_alternative<detail::cli::Null>(cell)) {
            out.isNull = true;
            out.done = true;
            return Result<detail::cli::GetDataChunk>(out);
        }
        // Serve string or binary cells as raw bytes from the per-column offset.
        const char* data = nullptr;
        std::size_t size = 0;
        if (const auto* str = std::get_if<std::string>(&cell)) {
            data = str->data();
            size = str->size();
        } else if (const auto* bin = std::get_if<std::vector<std::byte>>(&cell)) {
            data = reinterpret_cast<const char*>(bin->data());
            size = bin->size();
        } else {
            return Result<detail::cli::GetDataChunk>(rangeError());
        }
        if (s.lobOffsets.size() <= col) s.lobOffsets.resize(col + 1, 0);
        std::size_t& off = s.lobOffsets[col];
        std::size_t n = std::min(bufLen, size - off);
        if (lobChunkCap != 0) n = std::min(n, lobChunkCap);
        std::memcpy(buf, data + off, n);
        off += n;
        out.bytes = n;
        out.done = (off == size);
        return Result<detail::cli::GetDataChunk>(out);
    }

    Result<std::int64_t> executeStreaming(
        StatementHandle stmt, const std::vector<Value>& params,
        std::vector<detail::cli::ParamStreamSource> sources) override {
        ++executeStreamingCalls;
        (void)stmt;
        lastStreamValues = params;
        lastStreamedLobs.clear();
        if (!executeStreamingErrors.empty()) {
            Error e = executeStreamingErrors.front();
            executeStreamingErrors.pop_front();
            return Result<std::int64_t>(e);
        }
        std::vector<std::byte> stage(streamPullCap);
        for (const auto& src : sources) {
            std::vector<std::byte> all;
            for (;;) {
                std::size_t n = 0;
                try {
                    n = src.pull(stage.data(), stage.size());
                } catch (...) {  // faithful to the driver: never let it escape
                    Error e;
                    e.code = ErrorCode::Mapping;
                    e.message = "LOB source pull threw an exception";
                    return Result<std::int64_t>(e);
                }
                if (n == detail::cli::ParamStreamSource::npos) {
                    Error e;
                    e.code = ErrorCode::Mapping;
                    e.message = "LOB source pull failed";
                    return Result<std::int64_t>(e);
                }
                if (n > stage.size()) {  // would over-read the stage buffer
                    Error e;
                    e.code = ErrorCode::Mapping;
                    e.message = "LOB source returned an oversized chunk";
                    return Result<std::int64_t>(e);
                }
                if (n == 0) break;
                all.insert(all.end(), stage.begin(),
                           stage.begin() + static_cast<std::ptrdiff_t>(n));
            }
            lastStreamedLobs[src.paramIndex] = std::move(all);
        }
        if (!streamRowCounts.empty()) {
            std::int64_t n = streamRowCounts.front();
            streamRowCounts.pop_front();
            return Result<std::int64_t>(n);
        }
        return Result<std::int64_t>(std::int64_t{1});
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
