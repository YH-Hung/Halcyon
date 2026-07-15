#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "halcyon/isolation.hpp"
#include "halcyon/result.hpp"

namespace halcyon::detail::cli {

enum class ConnectionHandle : std::uint64_t { invalid = 0 };

// Opaque handle to a prepared statement owned by a driver implementation.
enum class StatementHandle : std::uint64_t { invalid = 0 };

struct ConnectionParams {
    std::string connectionString;
};

// Marker for SQL NULL inside a boundary Value.
struct Null {};
inline bool operator==(const Null&, const Null&) noexcept { return true; }
inline bool operator!=(const Null&, const Null&) noexcept { return false; }

// CLI-agnostic value exchanged across the seam: how the core passes parameters
// down and reads columns up without leaking sqlcli1.h C types above the seam.
// Integers arrive as int64; floating point as double; text as UTF-8 string;
// binary as bytes; SQL NULL as Null. bool is bind-only-distinct (a boolean
// column is read back as int64).
using Value = std::variant<Null, bool, std::int64_t, double, std::string,
                           std::vector<std::byte>>;

// Result of one chunked LOB read. bytes==0 with done==true is a clean EOF.
struct GetDataChunk {
    std::size_t bytes = 0;
    bool done = false;
    bool isNull = false;
};

// One data-at-exec parameter stream for executeStreaming.
struct ParamStreamSource {
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    std::size_t paramIndex = 0;  // 0-based '?' position
    // Fills buf with up to cap bytes; returns bytes written, 0 at EOF,
    // npos on source failure (aborts the execute).
    std::function<std::size_t(std::byte* buf, std::size_t cap)> pull;
    std::optional<std::uint64_t> sizeHint;  // total bytes if known
    bool isClob = false;                    // bind SQL_CLOB/SQL_C_CHAR instead of SQL_BLOB/SQL_C_BINARY
};

class ICliDriver {
public:
    virtual ~ICliDriver() = default;

    // --- Connection lifecycle (Plan 1) ---
    virtual Result<ConnectionHandle> connect(const ConnectionParams& params) = 0;
    virtual Result<void> disconnect(ConnectionHandle handle) = 0;
    virtual Result<bool> isAlive(ConnectionHandle handle) = 0;

    // --- Prepared-statement data path (Plan 2) ---

    // Prepares sql on conn; returns a non-invalid statement handle on success.
    virtual Result<StatementHandle> prepare(ConnectionHandle conn,
                                            const std::string& sql) = 0;

    // Binds positional parameters (vector index 0 == first '?') before execute.
    virtual Result<void> bindParams(StatementHandle stmt,
                                    const std::vector<Value>& params) = 0;

    // Binds `rows` as a column-wise parameter array and executes via array
    // binding, chunking internally to bound memory. Returns total rows affected,
    // summed across chunks. Precondition: `rows` is non-empty, rectangular, and
    // each column is type-homogeneous (validated above the seam). On failure
    // returns a single classified Error (SQLSTATE -> ErrorCode); each chunk is
    // atomic, and the failing row index is not recoverable from the driver.
    virtual Result<std::int64_t> executeBatch(
        StatementHandle stmt,
        const std::vector<std::vector<Value>>& rows) = 0;

    // Executes the statement. Returns rows affected for DML (>= 0); for a
    // result-set-producing statement the count is implementation-defined (0)
    // and the cursor becomes fetchable.
    virtual Result<std::int64_t> execute(StatementHandle stmt) = 0;

    // Number of columns in the active result set (0 when none).
    virtual Result<std::size_t> columnCount(StatementHandle stmt) = 0;

    // Name of the 0-based column (may be empty for unnamed expressions).
    virtual Result<std::string> columnName(StatementHandle stmt,
                                           std::size_t index) = 0;

    // Fetches up to maxRows rows from the open cursor as a row-major block of
    // neutral Values. Returns 1..maxRows rows on success; a short block does NOT
    // signal end. Returns an empty block exactly when the cursor is exhausted.
    // Returns a classified Error on a driver failure (e.g. mid-stream drop).
    virtual Result<std::vector<std::vector<Value>>> fetchBlock(
        StatementHandle stmt, std::size_t maxRows) = 0;

    // Releases a statement handle. Idempotent for already-finalized handles.
    virtual Result<void> finalize(StatementHandle stmt) = 0;

    // Closes any open cursor on stmt and resets it so it can be re-bound and
    // re-executed. Idempotent when no cursor is open. Required to reuse a cached
    // prepared statement that previously produced a result set.
    virtual Result<void> closeCursor(StatementHandle stmt) = 0;

    // --- Streaming data path (v1.1) ---
    // A statement's cursor is consumed EITHER via fetchBlock OR via
    // fetchNext+getValue+getDataChunk, never both (the two fetch styles must
    // not be mixed on one cursor).

    // Positions the cursor on the next row (no column buffers bound).
    // Returns false exactly when the cursor is exhausted.
    virtual Result<bool> fetchNext(StatementHandle stmt) = 0;

    // Reads one scalar cell of the current row as a neutral Value. Columns
    // must be read in ascending order across getValue/getDataChunk calls
    // (CLI SQLGetData constraint; enforced above the seam).
    virtual Result<Value> getValue(StatementHandle stmt, std::size_t col) = 0;

    // Reads the next chunk of a (LOB) cell into buf. Repeated calls walk the
    // cell; done=true accompanies the final chunk (or an empty chunk when the
    // cell is exhausted/NULL).
    virtual Result<GetDataChunk> getDataChunk(StatementHandle stmt,
                                              std::size_t col, std::byte* buf,
                                              std::size_t bufLen) = 0;

    // Executes with data-at-exec streams. `params` is the full positional
    // vector (streamed positions hold Null placeholders the driver ignores);
    // `sources` supplies those positions. Returns rows affected. On a source
    // failure (pull -> npos) the statement is cancelled and a Mapping error
    // returned; sources are single-use, so callers must never retry.
    virtual Result<std::int64_t> executeStreaming(
        StatementHandle stmt, const std::vector<Value>& params,
        std::vector<ParamStreamSource> sources) = 0;

    // --- Transaction control (Plan 4) ---

    // Enables/disables autocommit on the connection. A transaction begins by
    // disabling it and ends (commit/rollback) by re-enabling it.
    virtual Result<void> setAutoCommit(ConnectionHandle conn, bool enabled) = 0;

    // Sets the connection's transaction isolation attribute. Must not be
    // called while a transaction is open on the connection (CLI restriction);
    // callers set it immediately before begin and restore it after end.
    virtual Result<void> setIsolation(ConnectionHandle conn,
                                      Isolation level) = 0;

    // Commits / rolls back the current unit of work on the connection.
    virtual Result<void> commit(ConnectionHandle conn) = 0;
    virtual Result<void> rollback(ConnectionHandle conn) = 0;
};

}  // namespace halcyon::detail::cli
