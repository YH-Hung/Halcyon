#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

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

    // Advances the cursor. true => a row is available; false => end of result.
    virtual Result<bool> fetch(StatementHandle stmt) = 0;

    // Reads the current row's 0-based column as a neutral Value.
    virtual Result<Value> getColumn(StatementHandle stmt, std::size_t index) = 0;

    // Releases a statement handle. Idempotent for already-finalized handles.
    virtual Result<void> finalize(StatementHandle stmt) = 0;

    // Closes any open cursor on stmt and resets it so it can be re-bound and
    // re-executed. Idempotent when no cursor is open. Required to reuse a cached
    // prepared statement that previously produced a result set.
    virtual Result<void> closeCursor(StatementHandle stmt) = 0;

    // --- Transaction control (Plan 4) ---

    // Enables/disables autocommit on the connection. A transaction begins by
    // disabling it and ends (commit/rollback) by re-enabling it.
    virtual Result<void> setAutoCommit(ConnectionHandle conn, bool enabled) = 0;

    // Commits / rolls back the current unit of work on the connection.
    virtual Result<void> commit(ConnectionHandle conn) = 0;
    virtual Result<void> rollback(ConnectionHandle conn) = 0;
};

}  // namespace halcyon::detail::cli
