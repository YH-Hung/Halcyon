#include <sqlcli1.h>
#include <sqlext.h>

#include <cstddef>
#include <cstring>
#include <map>
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
        if (!cli_ok(SQLAllocHandle(SQL_HANDLE_DBC, env_, &dbc)))
            return make_error(SQL_HANDLE_ENV, env_, ErrorCode::Connection,
                              "alloc dbc");
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
        auto h = static_cast<ConnectionHandle>(++nextConn_);
        conns_[h] = dbc;
        return h;
    }

    Result<void> disconnect(ConnectionHandle handle) override {
        auto it = conns_.find(handle);
        if (it == conns_.end()) return Result<void>();
        SQLDisconnect(it->second);
        SQLFreeHandle(SQL_HANDLE_DBC, it->second);
        conns_.erase(it);
        return Result<void>();
    }

    Result<bool> isAlive(ConnectionHandle handle) override {
        auto it = conns_.find(handle);
        if (it == conns_.end()) return false;
        SQLHSTMT s = SQL_NULL_HANDLE;
        if (!cli_ok(SQLAllocHandle(SQL_HANDLE_STMT, it->second, &s))) return false;
        std::string sql = "SELECT 1 FROM SYSIBM.SYSDUMMY1";
        SQLRETURN rc = SQLExecDirect(s, reinterpret_cast<SQLCHAR*>(sql.data()),
                                     SQL_NTS);
        bool alive = cli_ok(rc);
        SQLFreeHandle(SQL_HANDLE_STMT, s);
        return alive;
    }

    Result<StatementHandle> prepare(ConnectionHandle conn,
                                    const std::string& sql) override {
        auto it = conns_.find(conn);
        if (it == conns_.end()) {
            Error e;
            e.code = ErrorCode::Connection;
            e.message = "unknown connection handle";
            return e;
        }
        SQLHSTMT s = SQL_NULL_HANDLE;
        if (!cli_ok(SQLAllocHandle(SQL_HANDLE_STMT, it->second, &s)))
            return make_error(SQL_HANDLE_DBC, it->second, ErrorCode::Unknown,
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
        auto h = static_cast<StatementHandle>(++nextStmt_);
        stmts_[h] = StmtState{s, {}};
        return h;
    }

    Result<void> bindParams(StatementHandle stmt,
                            const std::vector<Value>& params) override {
        auto& st = stmts_.at(stmt);
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
                bp.cType = SQL_C_SBIGINT;
                bp.sqlType = SQL_BIGINT;
                bp.i64 = *b ? 1 : 0;
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
        auto& st = stmts_.at(stmt);
        SQLRETURN rc = SQLExecute(st.handle);
        if (!cli_ok(rc) && rc != SQL_NO_DATA)
            return make_error(SQL_HANDLE_STMT, st.handle, ErrorCode::Unknown,
                              "SQLExecute failed");
        SQLLEN rows = 0;
        SQLRowCount(st.handle, &rows);
        return static_cast<std::int64_t>(rows < 0 ? 0 : rows);
    }

    Result<std::size_t> columnCount(StatementHandle stmt) override {
        SQLSMALLINT n = 0;
        if (!cli_ok(SQLNumResultCols(stmts_.at(stmt).handle, &n)))
            return make_error(SQL_HANDLE_STMT, stmts_.at(stmt).handle,
                              ErrorCode::Unknown, "SQLNumResultCols failed");
        return static_cast<std::size_t>(n < 0 ? 0 : n);
    }

    Result<std::string> columnName(StatementHandle stmt,
                                   std::size_t index) override {
        SQLCHAR name[256] = {0};
        SQLSMALLINT nameLen = 0, sqlType = 0, nullable = 0;
        SQLULEN colSize = 0;
        SQLSMALLINT scale = 0;
        SQLRETURN rc =
            SQLDescribeCol(stmts_.at(stmt).handle,
                           static_cast<SQLUSMALLINT>(index + 1), name,
                           sizeof(name), &nameLen, &sqlType, &colSize, &scale,
                           &nullable);
        if (!cli_ok(rc))
            return make_error(SQL_HANDLE_STMT, stmts_.at(stmt).handle,
                              ErrorCode::Unknown, "SQLDescribeCol failed");
        return std::string(reinterpret_cast<const char*>(name),
                           static_cast<std::size_t>(nameLen));
    }

    Result<bool> fetch(StatementHandle stmt) override {
        SQLRETURN rc = SQLFetch(stmts_.at(stmt).handle);
        if (rc == SQL_NO_DATA) return false;
        if (!cli_ok(rc))
            return make_error(SQL_HANDLE_STMT, stmts_.at(stmt).handle,
                              ErrorCode::Unknown, "SQLFetch failed");
        return true;
    }

    Result<Value> getColumn(StatementHandle stmt, std::size_t index) override {
        SQLHSTMT h = stmts_.at(stmt).handle;
        SQLUSMALLINT col = static_cast<SQLUSMALLINT>(index + 1);
        SQLCHAR name[1] = {0};
        SQLSMALLINT nameLen = 0, sqlType = 0, nullable = 0;
        SQLULEN colSize = 0;
        SQLSMALLINT scale = 0;
        if (!cli_ok(SQLDescribeCol(h, col, name, sizeof(name), &nameLen,
                                   &sqlType, &colSize, &scale, &nullable)))
            return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                              "SQLDescribeCol failed");

        switch (sqlType) {
            case SQL_SMALLINT:
            case SQL_INTEGER:
            case SQL_BIGINT: {
                SQLBIGINT v = 0;
                SQLLEN ind = 0;
                if (!cli_ok(SQLGetData(h, col, SQL_C_SBIGINT, &v, sizeof(v),
                                       &ind)))
                    return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                      "SQLGetData int failed");
                if (ind == SQL_NULL_DATA) return Value{Null{}};
                return Value{static_cast<std::int64_t>(v)};
            }
            case SQL_REAL:
            case SQL_FLOAT:
            case SQL_DOUBLE: {
                double v = 0;
                SQLLEN ind = 0;
                if (!cli_ok(SQLGetData(h, col, SQL_C_DOUBLE, &v, sizeof(v),
                                       &ind)))
                    return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                      "SQLGetData double failed");
                if (ind == SQL_NULL_DATA) return Value{Null{}};
                return Value{v};
            }
            default:
                // CHAR/VARCHAR/DECIMAL/NUMERIC/TIMESTAMP/DATE/TIME → UTF-8 string.
                return get_string(h, col);
        }
    }

    Result<void> finalize(StatementHandle stmt) override {
        auto it = stmts_.find(stmt);
        if (it == stmts_.end()) return Result<void>();
        SQLFreeHandle(SQL_HANDLE_STMT, it->second.handle);
        stmts_.erase(it);
        return Result<void>();
    }

    Result<void> setAutoCommit(ConnectionHandle conn, bool enabled) override {
        auto it = conns_.find(conn);
        if (it == conns_.end()) return unknown_conn();
        SQLPOINTER v = reinterpret_cast<SQLPOINTER>(static_cast<SQLLEN>(
            enabled ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF));
        SQLRETURN rc = SQLSetConnectAttr(it->second, SQL_ATTR_AUTOCOMMIT, v,
                                         SQL_IS_INTEGER);
        if (!cli_ok(rc))
            return make_error(SQL_HANDLE_DBC, it->second, ErrorCode::Unknown,
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
    struct StmtState {
        SQLHSTMT handle;
        std::vector<BoundParam> bound;
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
    Result<void> end_tran(ConnectionHandle conn, SQLSMALLINT how,
                          const char* context) {
        auto it = conns_.find(conn);
        if (it == conns_.end()) return unknown_conn();
        SQLRETURN rc = SQLEndTran(SQL_HANDLE_DBC, it->second, how);
        if (!cli_ok(rc))
            return make_error(SQL_HANDLE_DBC, it->second, ErrorCode::Unknown,
                              context);
        return Result<void>();
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

    SQLHENV env_ = SQL_NULL_HANDLE;
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
