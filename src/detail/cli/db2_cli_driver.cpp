#include <sqlcli1.h>
#include <sqlext.h>

#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "halcyon/detail/cli/db2_cli_driver.hpp"
#include "halcyon/detail/cli/sqlstate.hpp"
#include "halcyon/error.hpp"

namespace halcyon::detail::cli {
namespace {

bool cli_ok(SQLRETURN rc) {
    return rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO;
}

// Pulls the first diagnostic record off a handle and builds a classified Error.
Error make_error(SQLSMALLINT handleType, SQLHANDLE handle, ErrorCode fallback,
                 const char* context) {
    SQLCHAR state[6] = {0};
    SQLINTEGER native = 0;
    SQLCHAR msg[1024] = {0};
    SQLSMALLINT msgLen = 0;
    Error e;
    e.code = fallback;
    if (SQLGetDiagRec(handleType, handle, 1, state, &native, msg, sizeof(msg),
                      &msgLen) == SQL_SUCCESS ||
        msgLen > 0) {
        e.sqlstate = reinterpret_cast<const char*>(state);
        e.nativeError = static_cast<int>(native);
        e.message = reinterpret_cast<const char*>(msg);
        auto c = classify_sqlstate(e.sqlstate, e.nativeError);
        e.code = c.code;
        e.retriable = c.retriable;
    }
    if (e.message.empty()) e.message = context;
    return e;
}

// Maps a neutral Value's SQL type for SQLBindParameter.
struct BoundParam {
    SQLSMALLINT cType;
    SQLSMALLINT sqlType;
    SQLLEN length;          // SQL_NULL_DATA for null, else NTS handling
    std::vector<char> buf;  // backing storage kept alive until execute
    std::int64_t i64 = 0;
    double dbl = 0.0;
};

class Db2CliDriver final : public ICliDriver {
public:
    Db2CliDriver() {
        SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_);
    }
    ~Db2CliDriver() override {
        if (env_) SQLFreeHandle(SQL_HANDLE_ENV, env_);
    }

    Result<ConnectionHandle> connect(const ConnectionParams& params) override {
        SQLHDBC dbc = SQL_NULL_HANDLE;
        {
            // Allocating a DBC touches the shared env handle; serialize it.
            std::lock_guard<std::mutex> lk(mu_);
            if (!cli_ok(SQLAllocHandle(SQL_HANDLE_DBC, env_, &dbc)))
                return make_error(SQL_HANDLE_ENV, env_, ErrorCode::Connection,
                                  "alloc dbc");
        }
        // The dbc is private to this thread until it is published into conns_
        // below, so the (potentially slow, network-bound) connect runs unlocked.
        std::string dsn = params.connectionString;
        SQLCHAR out[1024];
        SQLSMALLINT outLen = 0;
        SQLRETURN rc = SQLDriverConnect(
            dbc, nullptr, reinterpret_cast<SQLCHAR*>(dsn.data()),
            static_cast<SQLSMALLINT>(dsn.size()), out, sizeof(out), &outLen,
            SQL_DRIVER_NOPROMPT);
        if (!cli_ok(rc)) {
            Error e = make_error(SQL_HANDLE_DBC, dbc, ErrorCode::Connection,
                                 "SQLDriverConnect failed");
            SQLFreeHandle(SQL_HANDLE_DBC, dbc);
            return e;
        }
        std::lock_guard<std::mutex> lk(mu_);
        auto h = static_cast<ConnectionHandle>(++nextConn_);
        conns_[h] = dbc;
        return h;
    }

    Result<void> disconnect(ConnectionHandle handle) override {
        SQLHDBC dbc;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = conns_.find(handle);
            if (it == conns_.end()) return Result<void>();
            dbc = it->second;
            conns_.erase(it);
        }
        SQLDisconnect(dbc);
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        return Result<void>();
    }

    Result<bool> isAlive(ConnectionHandle handle) override {
        SQLHDBC dbc = conn_handle(handle);
        if (dbc == SQL_NULL_HANDLE) return false;
        SQLHSTMT s = SQL_NULL_HANDLE;
        if (!cli_ok(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &s))) return false;
        std::string sql = "SELECT 1 FROM SYSIBM.SYSDUMMY1";
        SQLRETURN rc = SQLExecDirect(s, reinterpret_cast<SQLCHAR*>(sql.data()),
                                     SQL_NTS);
        bool alive = cli_ok(rc);
        SQLFreeHandle(SQL_HANDLE_STMT, s);
        return alive;
    }

    Result<StatementHandle> prepare(ConnectionHandle conn,
                                    const std::string& sql) override {
        SQLHDBC dbc = conn_handle(conn);
        if (dbc == SQL_NULL_HANDLE) {
            Error e;
            e.code = ErrorCode::Connection;
            e.message = "unknown connection handle";
            return e;
        }
        SQLHSTMT s = SQL_NULL_HANDLE;
        if (!cli_ok(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &s)))
            return make_error(SQL_HANDLE_DBC, dbc, ErrorCode::Unknown,
                              "alloc stmt");
        std::string mutableSql = sql;
        SQLRETURN rc = SQLPrepare(
            s, reinterpret_cast<SQLCHAR*>(mutableSql.data()), SQL_NTS);
        if (!cli_ok(rc)) {
            Error e = make_error(SQL_HANDLE_STMT, s, ErrorCode::Syntax,
                                 "SQLPrepare failed");
            SQLFreeHandle(SQL_HANDLE_STMT, s);
            return e;
        }
        std::lock_guard<std::mutex> lk(mu_);
        auto h = static_cast<StatementHandle>(++nextStmt_);
        stmts_[h] = StmtState{s, {}, std::nullopt, false};
        return h;
    }

    Result<void> bindParams(StatementHandle stmt,
                            const std::vector<Value>& params) override {
        StmtState* stp = stmt_state(stmt);
        if (!stp) return unknown_stmt();
        StmtState& st = *stp;
        st.bound.clear();
        st.bound.resize(params.size());
        for (std::size_t i = 0; i < params.size(); ++i) {
            BoundParam& bp = st.bound[i];
            const Value& v = params[i];
            if (std::holds_alternative<Null>(v)) {
                bp.cType = SQL_C_CHAR;
                bp.sqlType = SQL_VARCHAR;
                bp.length = SQL_NULL_DATA;
            } else if (auto* b = std::get_if<bool>(&v)) {
                // Bind as SQL_C_BIT (spec §7): a single 0/1 byte kept in buf,
                // bound through the generic buffer path below.
                bp.cType = SQL_C_BIT;
                bp.sqlType = SQL_BIT;
                bp.buf.assign(1, static_cast<char>(*b ? 1 : 0));
                bp.length = 0;
            } else if (auto* i64 = std::get_if<std::int64_t>(&v)) {
                bp.cType = SQL_C_SBIGINT;
                bp.sqlType = SQL_BIGINT;
                bp.i64 = *i64;
                bp.length = 0;
            } else if (auto* d = std::get_if<double>(&v)) {
                bp.cType = SQL_C_DOUBLE;
                bp.sqlType = SQL_DOUBLE;
                bp.dbl = *d;
                bp.length = 0;
            } else if (auto* s = std::get_if<std::string>(&v)) {
                bp.cType = SQL_C_CHAR;
                bp.sqlType = SQL_VARCHAR;
                bp.buf.assign(s->begin(), s->end());
                bp.length = static_cast<SQLLEN>(bp.buf.size());
            } else {
                const auto& bytes = std::get<std::vector<std::byte>>(v);
                bp.cType = SQL_C_BINARY;
                bp.sqlType = SQL_BINARY;
                bp.buf.resize(bytes.size());
                std::memcpy(bp.buf.data(), bytes.data(), bytes.size());
                bp.length = static_cast<SQLLEN>(bp.buf.size());
            }
            SQLPOINTER ptr;
            SQLLEN bufLen = 0;
            if (bp.cType == SQL_C_SBIGINT) {
                ptr = &bp.i64;
            } else if (bp.cType == SQL_C_DOUBLE) {
                ptr = &bp.dbl;
            } else {
                ptr = bp.buf.empty() ? const_cast<char*>("") : bp.buf.data();
                bufLen = static_cast<SQLLEN>(bp.buf.size());
            }
            SQLRETURN rc = SQLBindParameter(
                st.handle, static_cast<SQLUSMALLINT>(i + 1), SQL_PARAM_INPUT,
                bp.cType, bp.sqlType,
                bp.length == SQL_NULL_DATA ? 0 : static_cast<SQLULEN>(bufLen),
                0, ptr, bufLen, &bp.length);
            if (!cli_ok(rc))
                return make_error(SQL_HANDLE_STMT, st.handle, ErrorCode::Unknown,
                                  "SQLBindParameter failed");
        }
        return Result<void>();
    }

    Result<std::int64_t> execute(StatementHandle stmt) override {
        StmtState* st = stmt_state(stmt);
        if (!st) return unknown_stmt();
        SQLHSTMT h = st->handle;
        SQLRETURN rc = SQLExecute(h);
        if (!cli_ok(rc) && rc != SQL_NO_DATA)
            return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                              "SQLExecute failed");
        st->cols.reset();  // a fresh execute invalidates any cached describe
        st->blockFallback = false;
        SQLLEN rows = 0;
        SQLRowCount(h, &rows);
        return static_cast<std::int64_t>(rows < 0 ? 0 : rows);
    }

    Result<std::int64_t> executeBatch(
        StatementHandle stmt,
        const std::vector<std::vector<Value>>& rows) override {
        StmtState* stp = stmt_state(stmt);
        if (!stp) return unknown_stmt();
        SQLHSTMT h = stp->handle;
        const std::size_t nRows = rows.size();
        if (nRows == 0) return std::int64_t{0};
        const std::size_t nCols = rows.front().size();

        // Resolve each column's CLI type from its first non-null value. Input is
        // validated rectangular + homogeneous above the seam; an all-null column
        // defaults to VARCHAR (mirrors the scalar null path).
        std::vector<SQLSMALLINT> cType(nCols, SQL_C_CHAR);
        std::vector<SQLSMALLINT> sqlType(nCols, SQL_VARCHAR);
        std::vector<bool> isVar(nCols, true);
        std::vector<SQLLEN> fixedW(nCols, 0);
        for (std::size_t c = 0; c < nCols; ++c) {
            for (std::size_t r = 0; r < nRows; ++r) {
                const Value& v = rows[r][c];
                if (std::holds_alternative<Null>(v)) continue;
                if (std::holds_alternative<bool>(v)) {
                    cType[c] = SQL_C_BIT;
                    sqlType[c] = SQL_BIT;
                    isVar[c] = false;
                    fixedW[c] = 1;
                } else if (std::holds_alternative<std::int64_t>(v)) {
                    cType[c] = SQL_C_SBIGINT;
                    sqlType[c] = SQL_BIGINT;
                    isVar[c] = false;
                    fixedW[c] = sizeof(std::int64_t);
                } else if (std::holds_alternative<double>(v)) {
                    cType[c] = SQL_C_DOUBLE;
                    sqlType[c] = SQL_DOUBLE;
                    isVar[c] = false;
                    fixedW[c] = sizeof(double);
                } else if (std::holds_alternative<std::string>(v)) {
                    cType[c] = SQL_C_CHAR;
                    sqlType[c] = SQL_VARCHAR;
                    isVar[c] = true;
                } else {
                    cType[c] = SQL_C_BINARY;
                    sqlType[c] = SQL_BINARY;
                    isVar[c] = true;
                }
                break;  // first non-null defines the column
            }
        }

        constexpr std::size_t kByteBudget = 16u * 1024 * 1024;
        std::int64_t total = 0;
        std::size_t start = 0;
        while (start < nRows) {
            // Grow the chunk until adding the next row would exceed the budget.
            // colMax tracks each var-width column's longest element in the chunk.
            std::vector<std::size_t> colMax(nCols, 0);
            std::size_t end = start;
            while (end < nRows) {
                std::vector<std::size_t> newMax = colMax;
                for (std::size_t c = 0; c < nCols; ++c) {
                    if (!isVar[c]) continue;
                    const Value& v = rows[end][c];
                    std::size_t len = 0;
                    if (auto* s = std::get_if<std::string>(&v)) len = s->size();
                    else if (auto* b = std::get_if<std::vector<std::byte>>(&v))
                        len = b->size();
                    if (len > newMax[c]) newMax[c] = len;
                }
                const std::size_t prospectiveRows = end - start + 1;
                std::size_t bytes = 0;
                for (std::size_t c = 0; c < nCols; ++c) {
                    std::size_t w = isVar[c]
                                        ? (newMax[c] == 0 ? 1 : newMax[c])
                                        : static_cast<std::size_t>(fixedW[c]);
                    bytes += w * prospectiveRows;
                }
                if (end > start && bytes > kByteBudget) break;  // keep >= 1 row
                colMax = newMax;
                ++end;
            }
            const std::size_t chunkRows = end - start;

            // Column-wise buffers + indicator arrays; must outlive SQLExecute.
            // Each column's byte buffer is backed by std::int64_t storage so its
            // base is 8-byte aligned by construction: fixed-width
            // SQL_C_SBIGINT/SQL_C_DOUBLE slots (stride == 8) are then correctly
            // aligned for the CLI's typed reads, instead of relying on the
            // default-new alignment of a char array.
            std::vector<std::vector<std::int64_t>> buf(nCols);
            std::vector<std::vector<SQLLEN>> ind(nCols);
            std::vector<SQLLEN> width(nCols, 0);
            for (std::size_t c = 0; c < nCols; ++c) {
                width[c] = isVar[c]
                               ? static_cast<SQLLEN>(colMax[c] == 0 ? 1 : colMax[c])
                               : fixedW[c];
                const std::size_t nbytes =
                    static_cast<std::size_t>(width[c]) * chunkRows;
                buf[c].assign(
                    (nbytes + sizeof(std::int64_t) - 1) / sizeof(std::int64_t),
                    0);
                char* base = reinterpret_cast<char*>(buf[c].data());
                ind[c].assign(chunkRows, 0);
                for (std::size_t i = 0; i < chunkRows; ++i) {
                    const Value& v = rows[start + i][c];
                    char* slot = base + static_cast<std::size_t>(width[c]) * i;
                    if (std::holds_alternative<Null>(v)) {
                        ind[c][i] = SQL_NULL_DATA;
                    } else if (auto* b = std::get_if<bool>(&v)) {
                        *slot = static_cast<char>(*b ? 1 : 0);
                    } else if (auto* p = std::get_if<std::int64_t>(&v)) {
                        std::memcpy(slot, p, sizeof(*p));
                    } else if (auto* dd = std::get_if<double>(&v)) {
                        std::memcpy(slot, dd, sizeof(*dd));
                    } else if (auto* s = std::get_if<std::string>(&v)) {
                        std::memcpy(slot, s->data(), s->size());
                        ind[c][i] = static_cast<SQLLEN>(s->size());
                    } else {
                        const auto& by = std::get<std::vector<std::byte>>(v);
                        std::memcpy(slot, by.data(), by.size());
                        ind[c][i] = static_cast<SQLLEN>(by.size());
                    }
                }
            }

            SQLRETURN rcSet = SQLSetStmtAttr(  // NOLINT(performance-no-int-to-ptr)
                h, SQL_ATTR_PARAMSET_SIZE,
                reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(chunkRows)), 0);
            if (cli_ok(rcSet))
                rcSet = SQLSetStmtAttr(  // NOLINT(performance-no-int-to-ptr)
                    h, SQL_ATTR_PARAM_BIND_TYPE,
                    reinterpret_cast<SQLPOINTER>(
                        static_cast<SQLULEN>(SQL_PARAM_BIND_BY_COLUMN)),
                    0);
            if (!cli_ok(rcSet)) {
                Error e = make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                     "SQLSetStmtAttr (array binding) failed");
                reset_paramset(h);
                return e;
            }

            for (std::size_t c = 0; c < nCols; ++c) {
                SQLRETURN rc = SQLBindParameter(
                    h, static_cast<SQLUSMALLINT>(c + 1), SQL_PARAM_INPUT,
                    cType[c], sqlType[c], static_cast<SQLULEN>(width[c]), 0,
                    buf[c].data(), width[c], ind[c].data());
                if (!cli_ok(rc)) {
                    Error e = make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                         "SQLBindParameter (array) failed");
                    reset_paramset(h);
                    return e;
                }
            }

            SQLRETURN rc = SQLExecute(h);
            if (!cli_ok(rc) && rc != SQL_NO_DATA) {
                // Db2 applies each array chunk atomically: a failed chunk commits
                // nothing. The CLI driver does not expose which row failed (the
                // param-status array reports SQL_PARAM_DIAG_UNAVAILABLE and the
                // diagnostics carry no row number), so surface the Db2 diagnostic
                // (SQLSTATE) as-is. Wrap the call in a transaction for whole-batch
                // atomicity and re-drive on error.
                Error e = make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                     "SQLExecute (array binding) failed");
                reset_paramset(h);
                return e;
            }
            SQLLEN affected = 0;
            SQLRowCount(h, &affected);
            total += affected < 0 ? 0 : static_cast<std::int64_t>(affected);
            start = end;
        }
        // On success the reset must stick, or a later reuse of this cached
        // statement would execute with the wrong paramset size.
        if (!cli_ok(reset_paramset(h)))
            return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                              "SQLSetStmtAttr reset after array binding failed");
        return total;
    }

    Result<std::size_t> columnCount(StatementHandle stmt) override {
        StmtState* st = stmt_state(stmt);
        if (!st) return unknown_stmt();
        SQLHSTMT h = st->handle;
        SQLSMALLINT n = 0;
        if (!cli_ok(SQLNumResultCols(h, &n)))
            return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                              "SQLNumResultCols failed");
        return static_cast<std::size_t>(n < 0 ? 0 : n);
    }

    Result<std::string> columnName(StatementHandle stmt,
                                   std::size_t index) override {
        StmtState* st = stmt_state(stmt);
        if (!st) return unknown_stmt();
        SQLHSTMT h = st->handle;
        SQLCHAR name[256] = {0};
        SQLSMALLINT nameLen = 0, sqlType = 0, nullable = 0;
        SQLULEN colSize = 0;
        SQLSMALLINT scale = 0;
        SQLRETURN rc =
            SQLDescribeCol(h, static_cast<SQLUSMALLINT>(index + 1), name,
                           sizeof(name), &nameLen, &sqlType, &colSize, &scale,
                           &nullable);
        if (!cli_ok(rc))
            return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                              "SQLDescribeCol failed");
        return std::string(reinterpret_cast<const char*>(name),
                           static_cast<std::size_t>(nameLen));
    }

    Result<std::vector<std::vector<Value>>> fetchBlock(
        StatementHandle stmt, std::size_t maxRows) override {
        StmtState* st = stmt_state(stmt);
        if (!st) return unknown_stmt();
        if (maxRows == 0) return std::vector<std::vector<Value>>{};
        if (!st->cols) {
            if (auto d = describe_columns(st); !d.ok()) return d.error();
        }
        return st->blockFallback ? fetch_block_fallback(st, maxRows)
                                 : fetch_block_rowset(st, maxRows);
    }

    Result<void> finalize(StatementHandle stmt) override {
        SQLHSTMT h;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = stmts_.find(stmt);
            if (it == stmts_.end()) return Result<void>();
            h = it->second.handle;
            stmts_.erase(it);
        }
        SQLFreeHandle(SQL_HANDLE_STMT, h);
        return Result<void>();
    }

    Result<void> closeCursor(StatementHandle stmt) override {
        StmtState* st = stmt_state(stmt);
        if (!st) return Result<void>();  // unknown/finalized handle: no-op
        SQLRETURN rc = SQLFreeStmt(st->handle, SQL_CLOSE);
        // SQL_SUCCESS_WITH_INFO / "no cursor open" are benign; only a hard error
        // is reported. cli_ok() accepts SUCCESS and SUCCESS_WITH_INFO.
        if (!cli_ok(rc))
            return make_error(SQL_HANDLE_STMT, st->handle, ErrorCode::Unknown,
                              "SQLFreeStmt(SQL_CLOSE) failed");
        st->cols.reset();  // cursor closed -> re-describe on next fetch
        st->blockFallback = false;
        return Result<void>();
    }

    Result<void> setAutoCommit(ConnectionHandle conn, bool enabled) override {
        SQLHDBC dbc = conn_handle(conn);
        if (dbc == SQL_NULL_HANDLE) return unknown_conn();
        SQLPOINTER v = reinterpret_cast<SQLPOINTER>(static_cast<SQLLEN>(  // NOLINT(performance-no-int-to-ptr)
            enabled ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF));
        SQLRETURN rc = SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT, v,
                                         SQL_IS_INTEGER);
        if (!cli_ok(rc))
            return make_error(SQL_HANDLE_DBC, dbc, ErrorCode::Unknown,
                              "SQLSetConnectAttr(AUTOCOMMIT) failed");
        return Result<void>();
    }
    Result<void> commit(ConnectionHandle conn) override {
        return end_tran(conn, SQL_COMMIT, "SQLEndTran(COMMIT) failed");
    }
    Result<void> rollback(ConnectionHandle conn) override {
        return end_tran(conn, SQL_ROLLBACK, "SQLEndTran(ROLLBACK) failed");
    }

private:
    struct ColMeta {
        SQLSMALLINT sqlType = 0;
        SQLSMALLINT cType = SQL_C_CHAR;
        SQLLEN bindWidth = 0;  // fixed size, or buffer width for var columns
        bool bounded = true;
    };
    struct StmtState {
        SQLHSTMT handle;
        std::vector<BoundParam> bound;
        std::optional<std::vector<ColMeta>> cols;  // described once per cursor
        bool blockFallback = false;
    };

    static Error make_conn_error(const char* msg) {
        Error e;
        e.code = ErrorCode::Connection;
        e.message = msg;
        return e;
    }
    Result<void> unknown_conn() {
        return make_conn_error("unknown connection handle");
    }
    static Error unknown_stmt() {
        Error e;
        e.code = ErrorCode::Unknown;
        e.message = "unknown statement handle";
        return e;
    }

    // One-row cursor advance (was the body of fetch()).
    Result<bool> fetch_row(SQLHSTMT h) {
        SQLRETURN rc = SQLFetch(h);
        if (rc == SQL_NO_DATA) return false;
        if (!cli_ok(rc))
            return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                              "SQLFetch failed");
        return true;
    }

    static constexpr std::size_t kMaxBoundColBytes = 64u * 1024;

    // Reads the current row's 0-based column as a neutral Value using the cached
    // column SQL type (no per-cell SQLDescribeCol). Used by the fallback path.
    Result<Value> read_column_typed(SQLHSTMT h, std::size_t index,
                                    SQLSMALLINT sqlType) {
        SQLUSMALLINT col = static_cast<SQLUSMALLINT>(index + 1);
        switch (sqlType) {
            case SQL_SMALLINT:
            case SQL_INTEGER:
            case SQL_BIGINT: {
                SQLBIGINT v = 0;
                SQLLEN ind = 0;
                if (!cli_ok(SQLGetData(h, col, SQL_C_SBIGINT, &v, sizeof(v), &ind)))
                    return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                      "SQLGetData int failed");
                if (ind == SQL_NULL_DATA) return Value{Null{}};
                return Value{static_cast<std::int64_t>(v)};
            }
            case SQL_BIT: {
                SQLCHAR v = 0;
                SQLLEN ind = 0;
                if (!cli_ok(SQLGetData(h, col, SQL_C_BIT, &v, sizeof(v), &ind)))
                    return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                      "SQLGetData bit failed");
                if (ind == SQL_NULL_DATA) return Value{Null{}};
                return Value{static_cast<std::int64_t>(v ? 1 : 0)};
            }
            case SQL_REAL:
            case SQL_FLOAT:
            case SQL_DOUBLE: {
                double v = 0;
                SQLLEN ind = 0;
                if (!cli_ok(SQLGetData(h, col, SQL_C_DOUBLE, &v, sizeof(v), &ind)))
                    return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                      "SQLGetData double failed");
                if (ind == SQL_NULL_DATA) return Value{Null{}};
                return Value{v};
            }
            case SQL_BINARY:
            case SQL_VARBINARY:
            case SQL_LONGVARBINARY:
            case SQL_BLOB:
                return get_binary(h, col);
            default:
                return get_string(h, col);
        }
    }

    // Describes every result column once and caches type/width/bounded on the
    // statement. bounded=false (long/LOB or unknown/over-cap width) routes the
    // whole statement onto the per-row fallback.
    Result<void> describe_columns(StmtState* st) {
        SQLHSTMT h = st->handle;
        SQLSMALLINT n = 0;
        if (!cli_ok(SQLNumResultCols(h, &n)))
            return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                              "SQLNumResultCols failed");
        std::vector<ColMeta> cols(n < 0 ? 0 : static_cast<std::size_t>(n));
        bool anyUnbounded = false;
        for (std::size_t c = 0; c < cols.size(); ++c) {
            SQLCHAR name[1] = {0};
            SQLSMALLINT nameLen = 0, sqlType = 0, nullable = 0;
            SQLULEN colSize = 0;
            SQLSMALLINT scale = 0;
            if (!cli_ok(SQLDescribeCol(h, static_cast<SQLUSMALLINT>(c + 1), name,
                                       sizeof(name), &nameLen, &sqlType, &colSize,
                                       &scale, &nullable)))
                return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                  "SQLDescribeCol failed");
            ColMeta m;
            m.sqlType = sqlType;
            switch (sqlType) {
                case SQL_SMALLINT:
                case SQL_INTEGER:
                case SQL_BIGINT:
                    m.cType = SQL_C_SBIGINT;
                    m.bindWidth = sizeof(SQLBIGINT);
                    break;
                case SQL_REAL:
                case SQL_FLOAT:
                case SQL_DOUBLE:
                    m.cType = SQL_C_DOUBLE;
                    m.bindWidth = sizeof(double);
                    break;
                case SQL_BIT:
                    m.cType = SQL_C_BIT;
                    m.bindWidth = 1;
                    break;
                case SQL_BINARY:
                case SQL_VARBINARY:
                    m.cType = SQL_C_BINARY;
                    m.bindWidth = static_cast<SQLLEN>(colSize);
                    break;
                case SQL_LONGVARBINARY:
                case SQL_BLOB:
                case SQL_CLOB:
                case SQL_DBCLOB:
                case SQL_LONGVARCHAR:
                    m.bounded = false;  // long/LOB -> per-statement fallback
                    break;
                default:  // CHAR/VARCHAR/DECIMAL/NUMERIC/DATE/TIME/TIMESTAMP -> text
                    m.cType = SQL_C_CHAR;
                    m.bindWidth = static_cast<SQLLEN>(colSize) + 1;  // +1 for NUL
                    break;
            }
            if (m.bounded &&
                (m.bindWidth <= 0 ||
                 static_cast<std::size_t>(m.bindWidth) > kMaxBoundColBytes))
                m.bounded = false;  // unknown or too-wide -> fallback
            anyUnbounded = anyUnbounded || !m.bounded;
            cols[c] = m;
        }
        st->cols = std::move(cols);
        st->blockFallback = anyUnbounded;
        return Result<void>();
    }

    // Restores single-row state so a cached statement is clean for later reuse.
    static void reset_rowset(SQLHSTMT h) {
        SQLFreeStmt(h, SQL_UNBIND);
        SQLSetStmtAttr(  // NOLINT(performance-no-int-to-ptr)
            h, SQL_ATTR_ROW_ARRAY_SIZE,
            reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(1)), 0);
        SQLSetStmtAttr(h, SQL_ATTR_ROW_STATUS_PTR, nullptr, 0);
        SQLSetStmtAttr(h, SQL_ATTR_ROWS_FETCHED_PTR, nullptr, 0);
    }

    // Per-row fallback for long/LOB statements: identical Value output to the
    // rowset path, using the cached column SQL type.
    Result<std::vector<std::vector<Value>>> fetch_block_fallback(
        StmtState* st, std::size_t maxRows) {
        const auto& cols = *st->cols;
        std::vector<std::vector<Value>> out;
        while (out.size() < maxRows) {
            auto f = fetch_row(st->handle);
            if (!f.ok()) return f.error();
            if (!f.value()) break;  // clean end of cursor
            std::vector<Value> row;
            row.reserve(cols.size());
            for (std::size_t c = 0; c < cols.size(); ++c) {
                auto v = read_column_typed(st->handle, c, cols[c].sqlType);
                if (!v.ok()) return v.error();
                row.push_back(std::move(v.value()));
            }
            out.push_back(std::move(row));
        }
        return out;
    }

    // Column-wise rowset binding for all-bounded statements: bind once, one
    // SQLFetch fills a block of up to R rows, transpose to Values, then restore
    // single-row state so the cached handle is clean for later reuse. Consecutive
    // calls continue the same open cursor (each returns the next block; an empty
    // block signals end).
    Result<std::vector<std::vector<Value>>> fetch_block_rowset(
        StmtState* st, std::size_t maxRows) {
        SQLHSTMT h = st->handle;
        const auto& cols = *st->cols;
        const std::size_t nCols = cols.size();

        std::size_t perRow = 0;
        for (const auto& m : cols) perRow += static_cast<std::size_t>(m.bindWidth);
        constexpr std::size_t kFetchBlockBytes = 2u * 1024 * 1024;
        constexpr std::size_t kMaxBlockRows = 4096;
        std::size_t R = perRow == 0 ? kMaxBlockRows : (kFetchBlockBytes / perRow);
        if (R < 1) R = 1;
        if (R > kMaxBlockRows) R = kMaxBlockRows;
        if (R > maxRows) R = maxRows;

        // Column-wise buffers (int64-backed for 8-byte alignment) + indicators.
        std::vector<std::vector<std::int64_t>> buf(nCols);
        std::vector<std::vector<SQLLEN>> ind(nCols);
        for (std::size_t c = 0; c < nCols; ++c) {
            const std::size_t nbytes =
                static_cast<std::size_t>(cols[c].bindWidth) * R;
            buf[c].assign(
                (nbytes + sizeof(std::int64_t) - 1) / sizeof(std::int64_t), 0);
            ind[c].assign(R, 0);
        }

        SQLULEN rowsFetched = 0;
        std::vector<SQLUSMALLINT> rowStatus(R, 0);
        SQLSetStmtAttr(  // NOLINT(performance-no-int-to-ptr)
            h, SQL_ATTR_ROW_BIND_TYPE,
            reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(SQL_BIND_BY_COLUMN)),
            0);
        SQLSetStmtAttr(  // NOLINT(performance-no-int-to-ptr)
            h, SQL_ATTR_ROW_ARRAY_SIZE,
            reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(R)), 0);
        SQLSetStmtAttr(h, SQL_ATTR_ROW_STATUS_PTR, rowStatus.data(), 0);
        SQLSetStmtAttr(h, SQL_ATTR_ROWS_FETCHED_PTR, &rowsFetched, 0);

        for (std::size_t c = 0; c < nCols; ++c) {
            SQLRETURN rc = SQLBindCol(h, static_cast<SQLUSMALLINT>(c + 1),
                                      cols[c].cType, buf[c].data(),
                                      cols[c].bindWidth, ind[c].data());
            if (!cli_ok(rc)) {
                Error e = make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                     "SQLBindCol (rowset) failed");
                reset_rowset(h);
                return e;
            }
        }

        SQLRETURN rc = SQLFetch(h);
        if (rc == SQL_NO_DATA && rowsFetched == 0) {
            reset_rowset(h);
            return std::vector<std::vector<Value>>{};  // clean end of cursor
        }
        if (!cli_ok(rc) && rc != SQL_NO_DATA) {
            Error e = make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                 "SQLFetch (rowset) failed");
            reset_rowset(h);
            return e;
        }

        std::vector<std::vector<Value>> out;
        out.reserve(static_cast<std::size_t>(rowsFetched));
        for (std::size_t r = 0; r < static_cast<std::size_t>(rowsFetched); ++r) {
            std::vector<Value> row;
            row.reserve(nCols);
            for (std::size_t c = 0; c < nCols; ++c) {
                const char* base = reinterpret_cast<const char*>(buf[c].data());
                const char* slot =
                    base + static_cast<std::size_t>(cols[c].bindWidth) * r;
                const SQLLEN li = ind[c][r];
                if (li == SQL_NULL_DATA) {
                    row.push_back(Value{Null{}});
                    continue;
                }
                switch (cols[c].cType) {
                    case SQL_C_SBIGINT: {
                        SQLBIGINT v;
                        std::memcpy(&v, slot, sizeof(v));
                        row.push_back(Value{static_cast<std::int64_t>(v)});
                        break;
                    }
                    case SQL_C_DOUBLE: {
                        double v;
                        std::memcpy(&v, slot, sizeof(v));
                        row.push_back(Value{v});
                        break;
                    }
                    case SQL_C_BIT:
                        row.push_back(
                            Value{static_cast<std::int64_t>(*slot ? 1 : 0)});
                        break;
                    case SQL_C_BINARY:
                        row.push_back(Value{std::vector<std::byte>(
                            reinterpret_cast<const std::byte*>(slot),
                            reinterpret_cast<const std::byte*>(slot) +
                                static_cast<std::size_t>(li))});
                        break;
                    default:  // SQL_C_CHAR text (string/decimal/date/time/ts)
                        row.push_back(Value{
                            std::string(slot, static_cast<std::size_t>(li))});
                        break;
                }
            }
            out.push_back(std::move(row));
        }
        reset_rowset(h);
        return out;
    }

    // Locked lookups: copy a handle / grab a stable StmtState pointer under the
    // mutex, then operate on it unlocked. Map nodes are address-stable and a
    // statement/connection is single-owner per lease, so the returned handle is
    // safe to use without the lock while other threads work on other handles.
    SQLHDBC conn_handle(ConnectionHandle h) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = conns_.find(h);
        return it == conns_.end() ? SQL_NULL_HANDLE : it->second;
    }
    StmtState* stmt_state(StatementHandle h) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = stmts_.find(h);
        return it == stmts_.end() ? nullptr : &it->second;
    }

    Result<void> end_tran(ConnectionHandle conn, SQLSMALLINT how,
                          const char* context) {
        SQLHDBC dbc = conn_handle(conn);
        if (dbc == SQL_NULL_HANDLE) return unknown_conn();
        SQLRETURN rc = SQLEndTran(SQL_HANDLE_DBC, dbc, how);
        if (!cli_ok(rc))
            return make_error(SQL_HANDLE_DBC, dbc, ErrorCode::Unknown, context);
        return Result<void>();
    }

    // Restores single-row paramset state so a cached statement is clean for
    // later scalar bindParams/execute reuse. Returns the CLI status so the
    // success path can fail if the reset did not stick.
    static SQLRETURN reset_paramset(SQLHSTMT h) {
        return SQLSetStmtAttr(  // NOLINT(performance-no-int-to-ptr)
            h, SQL_ATTR_PARAMSET_SIZE,
            reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(1)), 0);
    }

    // Reads a (possibly long) character column by looping SQLGetData.
    Result<Value> get_string(SQLHSTMT h, SQLUSMALLINT col) {
        std::string out;
        char chunk[4096];
        SQLLEN ind = 0;
        for (;;) {
            SQLRETURN rc = SQLGetData(h, col, SQL_C_CHAR, chunk, sizeof(chunk),
                                      &ind);
            if (rc == SQL_NO_DATA) break;
            if (!cli_ok(rc))
                return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                  "SQLGetData char failed");
            if (ind == SQL_NULL_DATA) return Value{Null{}};
            std::size_t copied =
                (ind == SQL_NTS || ind >= static_cast<SQLLEN>(sizeof(chunk)))
                    ? std::strlen(chunk)
                    : static_cast<std::size_t>(ind);
            out.append(chunk, copied);
            if (rc == SQL_SUCCESS) break;  // no truncation: done
        }
        return Value{out};
    }

    // Reads a (possibly long) binary column by looping SQLGetData with
    // SQL_C_BINARY. Unlike get_string there is no NUL terminator: the indicator
    // gives the byte length, and on truncation (SQL_SUCCESS_WITH_INFO) it reports
    // the full remaining size (>= buffer) or SQL_NO_TOTAL, so the whole buffer was
    // filled — take all of it and loop until a SQL_SUCCESS chunk.
    Result<Value> get_binary(SQLHSTMT h, SQLUSMALLINT col) {
        std::vector<std::byte> out;
        char chunk[4096];
        SQLLEN ind = 0;
        for (;;) {
            SQLRETURN rc = SQLGetData(h, col, SQL_C_BINARY, chunk, sizeof(chunk),
                                      &ind);
            if (rc == SQL_NO_DATA) break;
            if (!cli_ok(rc))
                return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                  "SQLGetData binary failed");
            if (ind == SQL_NULL_DATA) return Value{Null{}};
            std::size_t copied =
                (ind == SQL_NO_TOTAL || ind >= static_cast<SQLLEN>(sizeof(chunk)))
                    ? sizeof(chunk)
                    : static_cast<std::size_t>(ind);
            const auto* bytes = reinterpret_cast<const std::byte*>(chunk);
            out.insert(out.end(), bytes, bytes + copied);
            if (rc == SQL_SUCCESS) break;  // last chunk delivered in full
        }
        return Value{std::move(out)};
    }

    SQLHENV env_ = SQL_NULL_HANDLE;
    // Guards the bookkeeping below (the handle maps + counters), which are shared
    // by every pooled connection through this one driver. Held only around map
    // lookups/inserts/erasures and counter bumps — never across a CLI round-trip
    // — so distinct connections still execute concurrently.
    std::mutex mu_;
    std::map<ConnectionHandle, SQLHDBC> conns_;
    std::map<StatementHandle, StmtState> stmts_;
    std::uint64_t nextConn_ = 0;
    std::uint64_t nextStmt_ = 0;
};

}  // namespace

std::unique_ptr<ICliDriver> make_db2_cli_driver() {
    return std::make_unique<Db2CliDriver>();
}

}  // namespace halcyon::detail::cli
