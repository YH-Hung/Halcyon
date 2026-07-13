#pragma once

namespace halcyon {

/// \brief Db2 transaction isolation levels.
///
/// Named with Db2's own terminology (UR/CS/RS/RR), not the SQL-standard
/// names, to avoid false equivalences — e.g. Db2 "Repeatable Read" maps to
/// the CLI's SERIALIZABLE constant. The mapping lives inside detail::cli.
enum class Isolation {
    UncommittedRead,  // UR
    CursorStability,  // CS (Db2's default)
    ReadStability,    // RS
    RepeatableRead,   // RR
};

}  // namespace halcyon
