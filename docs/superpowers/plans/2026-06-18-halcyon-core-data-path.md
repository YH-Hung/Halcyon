# Halcyon Core Data Path Implementation Plan (Plan 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Build the portable, mockable core data path on top of the Plan 1 seam:
extend `ICliDriver` to carry a neutral boundary `Value`, add the `TypeBinder<T>`
mapping layer, named/anonymous parameter binding, `Row`/`ResultSet` with
`row.as<...>()` iteration, `Connection`/`Statement` (`query`/`execute`), opt-in
`HALCYON_REFLECT` + `queryAs<T>`, SQLSTATE→`ErrorCode` classification, the **real**
`Db2CliDriver` over `sqlcli1.h`, and the first Dockerized Db2 integration tests.

**Architecture:** The seam (`detail::cli::ICliDriver`) is widened with prepared-
statement operations that exchange a CLI-agnostic `Value` (a small variant). The
core (`TypeBinder`, parameters, `Row`/`ResultSet`, `Connection`/`Statement`,
reflection) converts C++ types ↔ `Value` and never sees `sqlcli1.h`. Only
`src/detail/cli/db2_cli_driver.cpp` includes IBM CLI headers; it translates
`Value` ↔ Db2 C types and `SQLRETURN`/SQLSTATE → `halcyon::Error`. This keeps the
**invariant** from `AGENTS.md`: nothing above the seam references `sqlcli1.h`.

**Tech Stack:** C++17, CMake ≥ 3.20, GoogleTest (FetchContent, from Plan 1), the
user-supplied IBM Db2 CLI driver at `third_party/clidriver` (linked into the library
for the first time in this plan via `DB2::CLI`), Docker for the gated integration
suite.

**Builds on (Plan 1, merged):** `include/halcyon/{version,error,result}.hpp`,
`include/halcyon/detail/cli/driver.hpp` (the seam), `tests/unit/mock_cli_driver.hpp`,
the CMake target `halcyon::halcyon`, and the GoogleTest harness.

---

## Design contracts (shared across tasks — keep consistent)

These names/signatures are referenced by multiple tasks. Treat them as the source
of truth; if a later task needs a change, update this section in the same commit.

**Neutral boundary value (Task 1, `detail/cli/driver.hpp`):**

```cpp
struct Null {};
using Value = std::variant<Null, bool, std::int64_t, double, std::string,
                           std::vector<std::byte>>;
```

- Integers cross the seam as `std::int64_t`; floating point as `double`; text as
  UTF-8 `std::string`; binary as `std::vector<std::byte>`; SQL NULL as `Null`.
  `bool` is distinct for binding; on *read* a boolean column comes back as
  `std::int64_t` (Db2 has no native BIT result), so `TypeBinder<bool>::from_value`
  accepts both.

**Seam additions to `ICliDriver` (Task 1):** `prepare`, `bindParams`, `execute`
(returns rows affected), `columnCount`, `columnName`, `fetch`, `getColumn`,
`finalize` — all `Result<...>`-returning, all keyed by an opaque
`StatementHandle` (Task 1).

**Type layer (Task 2, `types.hpp`):** `TypeBinder<T>` with
`static Value to_value(const T&)` / `static Result<T> from_value(const Value&)`;
traits `is_bindable<T>` / `is_readable<T>`; free helpers
`detail::to_value(const T&)` (handles bind-only `std::string_view`/`const char*`)
and `detail::from_value<T>(const Value&)`; `detail::mapping_error(std::string)`.

**Error classification (Task 7, `detail/cli/sqlstate.hpp`):**
`Classification classify_sqlstate(std::string_view sqlstate, int nativeError)`.

---

## File Structure (created/modified by this plan)

- `include/halcyon/detail/cli/driver.hpp` — **modify**: add `Value`, `Null`,
  `StatementHandle`, statement methods on `ICliDriver`.
- `include/halcyon/types.hpp` — **create**: `TypeBinder<T>`, traits, `HALCYON_REFLECT`.
- `include/halcyon/parameters.hpp` — **create**: anonymous packing + `params` named binding.
- `include/halcyon/connection.hpp` — **create**: `Row`, `ResultSet`, `Statement`, `Connection`.
- `include/halcyon/detail/cli/sqlstate.hpp` — **create**: SQLSTATE→`ErrorCode` (portable).
- `include/halcyon/detail/cli/db2_cli_driver.hpp` — **create**: factory `make_db2_cli_driver()` (no `sqlcli1.h`).
- `include/halcyon/halcyon.hpp` — **create**: umbrella header.
- `src/detail/cli/sqlstate.cpp` — **create**: classification impl (portable).
- `src/detail/cli/db2_cli_driver.cpp` — **create**: the only `sqlcli1.h` translation unit.
- `tests/unit/mock_cli_driver.hpp` — **modify**: scriptable statement engine.
- `tests/unit/test_cli_seam.cpp` — **modify**: add statement-seam tests.
- `tests/unit/test_type_binder.cpp` — **create**.
- `tests/unit/test_parameters.cpp` — **create**.
- `tests/unit/test_result_set.cpp` — **create**.
- `tests/unit/test_connection.cpp` — **create**.
- `tests/unit/test_reflect.cpp` — **create**.
- `tests/unit/test_sqlstate.cpp` — **create**.
- `tests/integration/CMakeLists.txt`, `tests/integration/test_db2_roundtrip.cpp` — **create**.
- `docker/docker-compose.yml` — **create**: Db2 community container for integration.
- `CMakeLists.txt` — **modify**: link `DB2::CLI`, add core sources, integration option.
- `tests/CMakeLists.txt` — **modify**: register new unit tests + integration subdir.

---

## Task 1: Extend the CLI seam with a neutral `Value` and statement operations

**Files:**
- Modify: `include/halcyon/detail/cli/driver.hpp`
- Modify: `tests/unit/mock_cli_driver.hpp`
- Modify: `tests/unit/test_cli_seam.cpp`

- [x] **Step 1: Write the failing seam tests**

Append to `tests/unit/test_cli_seam.cpp` (keep the existing four tests):

```cpp
#include <cstddef>
#include <vector>

using halcyon::detail::cli::Null;
using halcyon::detail::cli::StatementHandle;
using halcyon::detail::cli::Value;

TEST(CliSeamStatement, PrepareBindExecuteReturnsRowsAffected) {
    MockCliDriver driver;
    auto conn = driver.connect(ConnectionParams{"x"}).value();
    driver.execRowCounts.push_back(3);

    auto st = driver.prepare(conn, "UPDATE t SET a = ? WHERE b = ?");
    ASSERT_TRUE(st.ok());
    ASSERT_TRUE(driver.bindParams(st.value(),
                                  {Value{std::int64_t{7}}, Value{std::string{"k"}}})
                    .ok());
    auto rows = driver.execute(st.value());
    ASSERT_TRUE(rows.ok());
    EXPECT_EQ(rows.value(), 3);

    ASSERT_EQ(driver.statements.at(st.value()).boundParams.size(), 2u);
    EXPECT_EQ(driver.statements.at(st.value()).boundParams[0], Value{std::int64_t{7}});
    EXPECT_EQ(driver.preparedSql.back(), "UPDATE t SET a = ? WHERE b = ?");
    ASSERT_TRUE(driver.finalize(st.value()).ok());
}

TEST(CliSeamStatement, ScriptedResultSetIsFetchable) {
    MockCliDriver driver;
    auto conn = driver.connect(ConnectionParams{"x"}).value();
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id", "name"},
        {{Value{std::int64_t{1}}, Value{std::string{"ada"}}},
         {Value{std::int64_t{2}}, Value{Null{}}}}});

    auto st = driver.prepare(conn, "SELECT id, name FROM t").value();
    ASSERT_TRUE(driver.execute(st).ok());
    EXPECT_EQ(driver.columnCount(st).value(), 2u);
    EXPECT_EQ(driver.columnName(st, 1).value(), "name");

    ASSERT_TRUE(driver.fetch(st).value());
    EXPECT_EQ(driver.getColumn(st, 0).value(), Value{std::int64_t{1}});
    EXPECT_EQ(driver.getColumn(st, 1).value(), Value{std::string{"ada"}});
    ASSERT_TRUE(driver.fetch(st).value());
    EXPECT_EQ(driver.getColumn(st, 1).value(), Value{Null{}});
    EXPECT_FALSE(driver.fetch(st).value());  // end of cursor
}

TEST(CliSeamStatement, PrepareErrorIsScriptable) {
    MockCliDriver driver;
    auto conn = driver.connect(ConnectionParams{"x"}).value();
    Error e;
    e.code = ErrorCode::Syntax;
    e.sqlstate = "42601";
    driver.prepareErrors.push_back(e);
    auto st = driver.prepare(conn, "SELEKT 1");
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().code, ErrorCode::Syntax);
}
```

- [x] **Step 2: Extend the seam header**

Edit `include/halcyon/detail/cli/driver.hpp`. Add includes and, after
`ConnectionParams`, the boundary value + statement surface:

```cpp
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
};

}  // namespace halcyon::detail::cli
```

- [x] **Step 3: Rewrite the mock as a scriptable statement engine**

Replace the body of `tests/unit/mock_cli_driver.hpp` (keep the existing
connect/disconnect/isAlive scripting fields and behavior):

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
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

    std::vector<std::string> preparedSql;
    std::map<StatementHandle, StmtState> statements;

    Result<ConnectionHandle> connect(const ConnectionParams& params) override {
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

    Result<std::int64_t> execute(StatementHandle stmt) override {
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

    Result<bool> fetch(StatementHandle stmt) override {
        auto& s = statements.at(stmt);
        ++s.position;
        return Result<bool>(s.position <
                            static_cast<long>(s.cursor.rows.size()));
    }

    Result<Value> getColumn(StatementHandle stmt, std::size_t index) override {
        auto& s = statements.at(stmt);
        if (s.position < 0 ||
            s.position >= static_cast<long>(s.cursor.rows.size())) {
            return Result<Value>(rangeError());
        }
        const auto& row = s.cursor.rows[static_cast<std::size_t>(s.position)];
        if (index >= row.size()) return Result<Value>(rangeError());
        return Result<Value>(row[index]);
    }

    Result<void> finalize(StatementHandle stmt) override {
        statements.erase(stmt);
        return Result<void>();
    }

private:
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
```

- [x] **Step 4: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R CliSeam --output-on-failure
```

Expected: original `CliSeam.*` tests still PASS and the three new
`CliSeamStatement.*` tests PASS. (No `tests/CMakeLists.txt` change needed —
`test_cli_seam.cpp` is already registered.)

- [x] **Step 5: Commit**

```bash
git add include/halcyon/detail/cli/driver.hpp tests/unit/mock_cli_driver.hpp \
        tests/unit/test_cli_seam.cpp
git commit -m "feat: extend CLI seam with boundary Value and statement operations"
```

---

## Task 2: `TypeBinder<T>` mapping layer

**Files:**
- Create: `include/halcyon/types.hpp`
- Modify: `tests/CMakeLists.txt` (add `unit/test_type_binder.cpp`)
- Test: `tests/unit/test_type_binder.cpp`

- [x] **Step 1: Write the failing test**

Create `tests/unit/test_type_binder.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "halcyon/types.hpp"

using halcyon::ErrorCode;
using halcyon::TypeBinder;
using halcyon::detail::cli::Null;
using halcyon::detail::cli::Value;

TEST(TypeBinder, IntegerRoundTripAndNarrowing) {
    EXPECT_EQ(TypeBinder<std::int64_t>::to_value(std::int64_t{5}),
              Value{std::int64_t{5}});
    EXPECT_EQ(TypeBinder<std::int32_t>::from_value(Value{std::int64_t{42}}).value(),
              42);
    // Out-of-range narrowing is a Mapping error, never silent truncation.
    auto bad = TypeBinder<std::int32_t>::from_value(
        Value{std::int64_t{1} << 40});
    ASSERT_FALSE(bad.ok());
    EXPECT_EQ(bad.error().code, ErrorCode::Mapping);
}

TEST(TypeBinder, BoolReadsFromBoolOrInt) {
    EXPECT_TRUE(TypeBinder<bool>::from_value(Value{true}).value());
    EXPECT_TRUE(TypeBinder<bool>::from_value(Value{std::int64_t{1}}).value());
    EXPECT_FALSE(TypeBinder<bool>::from_value(Value{std::int64_t{0}}).value());
}

TEST(TypeBinder, DoubleAndString) {
    EXPECT_EQ(TypeBinder<double>::from_value(Value{2.5}).value(), 2.5);
    EXPECT_EQ(TypeBinder<std::string>::from_value(Value{std::string{"hi"}}).value(),
              "hi");
}

TEST(TypeBinder, NullIntoNonOptionalIsMappingError) {
    auto r = TypeBinder<std::int64_t>::from_value(Value{Null{}});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Mapping);
}

TEST(TypeBinder, OptionalMapsNullToNullopt) {
    auto none = TypeBinder<std::optional<int>>::from_value(Value{Null{}});
    ASSERT_TRUE(none.ok());
    EXPECT_FALSE(none.value().has_value());

    auto some = TypeBinder<std::optional<int>>::from_value(Value{std::int64_t{9}});
    ASSERT_TRUE(some.ok());
    ASSERT_TRUE(some.value().has_value());
    EXPECT_EQ(*some.value(), 9);

    EXPECT_TRUE(std::holds_alternative<Null>(
        TypeBinder<std::optional<int>>::to_value(std::nullopt)));
}

TEST(TypeBinder, TypeMismatchIsMappingError) {
    auto r = TypeBinder<std::int64_t>::from_value(Value{std::string{"x"}});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Mapping);
}

TEST(TypeBinder, BindOnlyStringViewAndCharPtr) {
    EXPECT_EQ(halcyon::detail::to_value(std::string_view{"abc"}),
              Value{std::string{"abc"}});
    EXPECT_EQ(halcyon::detail::to_value("lit"), Value{std::string{"lit"}});
}

TEST(TypeBinder, Traits) {
    static_assert(halcyon::is_bindable<int>::value);
    static_assert(halcyon::is_readable<int>::value);
    static_assert(halcyon::is_bindable<std::optional<std::string>>::value);
}
```

- [x] **Step 2: Run test to verify it fails**

Add `unit/test_type_binder.cpp` to `tests/CMakeLists.txt` (Step 4 below shows the
final list), reconfigure, build. Expected: compile FAIL — `halcyon/types.hpp`
not found.

- [x] **Step 3: Write the types header**

Create `include/halcyon/types.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/error.hpp"
#include "halcyon/result.hpp"

namespace halcyon {

namespace detail {

inline Error mapping_error(std::string message) {
    Error e;
    e.code = ErrorCode::Mapping;
    e.message = std::move(message);
    return e;
}

}  // namespace detail

// Primary template is declared (not defined): a missing specialization yields a
// clear "incomplete type" error and disables the traits below via SFINAE.
template <class T, class Enable = void>
struct TypeBinder;

// --- integral (excluding bool) ---
template <class T>
struct TypeBinder<T, std::enable_if_t<std::is_integral_v<T> &&
                                      !std::is_same_v<T, bool>>> {
    static detail::cli::Value to_value(T v) {
        return detail::cli::Value{static_cast<std::int64_t>(v)};
    }
    static Result<T> from_value(const detail::cli::Value& v) {
        if (std::holds_alternative<detail::cli::Null>(v))
            return detail::mapping_error("NULL into non-optional integral");
        if (auto* p = std::get_if<std::int64_t>(&v)) {
            if (!in_range(*p))
                return detail::mapping_error("integer out of range for target type");
            return static_cast<T>(*p);
        }
        if (auto* b = std::get_if<bool>(&v)) return static_cast<T>(*b ? 1 : 0);
        return detail::mapping_error("type mismatch: expected integer");
    }

private:
    static bool in_range(std::int64_t v) {
        using L = std::numeric_limits<T>;
        if constexpr (std::is_signed_v<T>) {
            return v >= static_cast<std::int64_t>(L::min()) &&
                   v <= static_cast<std::int64_t>(L::max());
        } else {
            return v >= 0 &&
                   static_cast<std::uint64_t>(v) <=
                       static_cast<std::uint64_t>(L::max());
        }
    }
};

// --- bool ---
template <>
struct TypeBinder<bool> {
    static detail::cli::Value to_value(bool v) { return detail::cli::Value{v}; }
    static Result<bool> from_value(const detail::cli::Value& v) {
        if (auto* b = std::get_if<bool>(&v)) return *b;
        if (auto* i = std::get_if<std::int64_t>(&v)) return *i != 0;
        if (std::holds_alternative<detail::cli::Null>(v))
            return detail::mapping_error("NULL into non-optional bool");
        return detail::mapping_error("type mismatch: expected bool");
    }
};

// --- floating point ---
template <class T>
struct TypeBinder<T, std::enable_if_t<std::is_floating_point_v<T>>> {
    static detail::cli::Value to_value(T v) {
        return detail::cli::Value{static_cast<double>(v)};
    }
    static Result<T> from_value(const detail::cli::Value& v) {
        if (auto* d = std::get_if<double>(&v)) return static_cast<T>(*d);
        if (auto* i = std::get_if<std::int64_t>(&v)) return static_cast<T>(*i);
        if (std::holds_alternative<detail::cli::Null>(v))
            return detail::mapping_error("NULL into non-optional floating point");
        return detail::mapping_error("type mismatch: expected floating point");
    }
};

// --- std::string ---
template <>
struct TypeBinder<std::string> {
    static detail::cli::Value to_value(std::string v) {
        return detail::cli::Value{std::move(v)};
    }
    static Result<std::string> from_value(const detail::cli::Value& v) {
        if (auto* s = std::get_if<std::string>(&v)) return *s;
        if (std::holds_alternative<detail::cli::Null>(v))
            return detail::mapping_error("NULL into non-optional string");
        return detail::mapping_error("type mismatch: expected string");
    }
};

// --- std::vector<std::byte> (binary) ---
template <>
struct TypeBinder<std::vector<std::byte>> {
    static detail::cli::Value to_value(std::vector<std::byte> v) {
        return detail::cli::Value{std::move(v)};
    }
    static Result<std::vector<std::byte>> from_value(const detail::cli::Value& v) {
        if (auto* b = std::get_if<std::vector<std::byte>>(&v)) return *b;
        if (std::holds_alternative<detail::cli::Null>(v))
            return detail::mapping_error("NULL into non-optional binary");
        return detail::mapping_error("type mismatch: expected binary");
    }
};

// --- bind-only std::string_view ---
template <>
struct TypeBinder<std::string_view> {
    static detail::cli::Value to_value(std::string_view v) {
        return detail::cli::Value{std::string{v}};
    }
    // intentionally no from_value: read into std::string instead.
};

// --- std::optional<T> (nullability) ---
template <class U>
struct TypeBinder<std::optional<U>> {
    static detail::cli::Value to_value(const std::optional<U>& o) {
        if (o) return TypeBinder<U>::to_value(*o);
        return detail::cli::Value{detail::cli::Null{}};
    }
    static Result<std::optional<U>> from_value(const detail::cli::Value& v) {
        if (std::holds_alternative<detail::cli::Null>(v))
            return std::optional<U>{};
        auto r = TypeBinder<U>::from_value(v);
        if (!r.ok()) return r.error();
        return std::optional<U>{std::move(r.value())};
    }
};

// --- traits ---
template <class T, class Enable = void>
struct is_bindable : std::false_type {};
template <class T>
struct is_bindable<T, std::void_t<decltype(TypeBinder<std::decay_t<T>>::to_value(
                          std::declval<const std::decay_t<T>&>()))>>
    : std::true_type {};

template <class T, class Enable = void>
struct is_readable : std::false_type {};
template <class T>
struct is_readable<T, std::void_t<decltype(TypeBinder<std::decay_t<T>>::from_value(
                          std::declval<const detail::cli::Value&>()))>>
    : std::true_type {};

namespace detail {

// Bind helper that also accepts string literals / string_view by value.
template <class T>
detail::cli::Value to_value(const T& v) {
    using D = std::decay_t<T>;
    if constexpr (std::is_convertible_v<const T&, std::string_view> &&
                  !std::is_same_v<D, std::string>) {
        return detail::cli::Value{std::string{std::string_view(v)}};
    } else {
        return TypeBinder<D>::to_value(v);
    }
}

template <class T>
Result<std::decay_t<T>> from_value(const detail::cli::Value& v) {
    return TypeBinder<std::decay_t<T>>::from_value(v);
}

}  // namespace detail

}  // namespace halcyon
```

- [x] **Step 4: Wire into the build**

In `tests/CMakeLists.txt`, extend the test sources:

```cmake
add_executable(halcyon_unit_tests
    unit/test_version.cpp
    unit/test_error.cpp
    unit/test_result.cpp
    unit/test_cli_seam.cpp
    unit/test_type_binder.cpp)
```

- [x] **Step 5: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R TypeBinder --output-on-failure
```

Expected: all `TypeBinder.*` tests PASS.

- [x] **Step 6: Commit**

```bash
git add include/halcyon/types.hpp tests/CMakeLists.txt tests/unit/test_type_binder.cpp
git commit -m "feat: add TypeBinder<T> mapping layer with nullability and traits"
```

---

## Task 3: Parameter binding (anonymous + named)

**Files:**
- Create: `include/halcyon/parameters.hpp`
- Modify: `tests/CMakeLists.txt` (add `unit/test_parameters.cpp`)
- Test: `tests/unit/test_parameters.cpp`

- [x] **Step 1: Write the failing test**

Create `tests/unit/test_parameters.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "halcyon/parameters.hpp"

using halcyon::params;
using halcyon::ErrorCode;
using halcyon::detail::cli::Value;

TEST(Parameters, PacksAnonymousArgsInOrder) {
    auto v = halcyon::detail::pack_params(21, std::string{"NYC"}, true);
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], Value{std::int64_t{21}});
    EXPECT_EQ(v[1], Value{std::string{"NYC"}});
    EXPECT_EQ(v[2], Value{true});
}

TEST(Parameters, PacksStringLiteral) {
    auto v = halcyon::detail::pack_params("lit");
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], Value{std::string{"lit"}});
}

TEST(Parameters, NamedRewriteProducesPositionalSqlAndValues) {
    auto r = halcyon::detail::bind_named(
        "SELECT id FROM u WHERE age > :age AND city = :city",
        params{{"age", 21}, {"city", std::string{"NYC"}}});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().sql, "SELECT id FROM u WHERE age > ? AND city = ?");
    ASSERT_EQ(r.value().params.size(), 2u);
    EXPECT_EQ(r.value().params[0], Value{std::int64_t{21}});
    EXPECT_EQ(r.value().params[1], Value{std::string{"NYC"}});
}

TEST(Parameters, NamedSupportsRepeatedPlaceholder) {
    auto r = halcyon::detail::bind_named("VALUES (:x, :x)", params{{"x", 5}});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().sql, "VALUES (?, ?)");
    ASSERT_EQ(r.value().params.size(), 2u);
    EXPECT_EQ(r.value().params[1], Value{std::int64_t{5}});
}

TEST(Parameters, UnknownNamedParamIsMappingError) {
    auto r = halcyon::detail::bind_named("WHERE a = :missing", params{{"x", 1}});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Mapping);
}

TEST(Parameters, DoubleColonIsLeftLiteral) {
    auto r = halcyon::detail::bind_named("SELECT a::int FROM t", params{});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().sql, "SELECT a::int FROM t");
    EXPECT_TRUE(r.value().params.empty());
}
```

- [x] **Step 2: Run test to verify it fails**

Add `unit/test_parameters.cpp` to `tests/CMakeLists.txt`, reconfigure, build.
Expected: compile FAIL — `halcyon/parameters.hpp` not found.

- [x] **Step 3: Write the parameters header**

Create `include/halcyon/parameters.hpp`:

```cpp
#pragma once

#include <cctype>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/result.hpp"
#include "halcyon/types.hpp"

namespace halcyon {

// One named binding; constructible from any bindable value (erased to Value).
struct NamedParam {
    std::string name;
    detail::cli::Value value;

    template <class T>
    NamedParam(std::string n, const T& v)  // NOLINT(google-explicit-constructor)
        : name(std::move(n)), value(detail::to_value(v)) {}
};

// Collection of named bindings: params{{"age", 21}, {"city", "NYC"}}.
class params {
public:
    params() = default;
    params(std::initializer_list<NamedParam> xs)  // NOLINT
        : items_(xs) {}

    const std::vector<NamedParam>& items() const noexcept { return items_; }

private:
    std::vector<NamedParam> items_;
};

namespace detail {

// Packs variadic anonymous arguments into a positional Value vector.
template <class... Args>
std::vector<detail::cli::Value> pack_params(const Args&... args) {
    return std::vector<detail::cli::Value>{detail::to_value(args)...};
}

struct PreparedSql {
    std::string sql;                       // ':name' rewritten to positional '?'
    std::vector<detail::cli::Value> params;
};

// Rewrites ':name' placeholders to '?' in appearance order, resolving each
// against p. A repeated name binds its value again. '::' is treated literally.
// Note (v1 limitation): this scanner does not skip string/comment literals.
inline Result<PreparedSql> bind_named(const std::string& sql, const params& p) {
    auto find = [&](const std::string& name) -> const detail::cli::Value* {
        for (const auto& it : p.items())
            if (it.name == name) return &it.value;
        return nullptr;
    };

    PreparedSql out;
    out.sql.reserve(sql.size());
    for (std::size_t i = 0; i < sql.size();) {
        char c = sql[i];
        if (c == ':' && i + 1 < sql.size() && sql[i + 1] == ':') {
            out.sql += "::";
            i += 2;
            continue;
        }
        const bool starts_name =
            c == ':' && i + 1 < sql.size() &&
            (std::isalpha(static_cast<unsigned char>(sql[i + 1])) ||
             sql[i + 1] == '_');
        if (!starts_name) {
            out.sql += c;
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        while (j < sql.size() &&
               (std::isalnum(static_cast<unsigned char>(sql[j])) ||
                sql[j] == '_')) {
            ++j;
        }
        std::string name = sql.substr(i + 1, j - (i + 1));
        const detail::cli::Value* v = find(name);
        if (v == nullptr)
            return mapping_error("unknown named parameter ':" + name + "'");
        out.sql += '?';
        out.params.push_back(*v);
        i = j;
    }
    return out;
}

}  // namespace detail

}  // namespace halcyon
```

- [x] **Step 4: Wire into the build**

Add `unit/test_parameters.cpp` to `halcyon_unit_tests` in `tests/CMakeLists.txt`.

- [x] **Step 5: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R Parameters --output-on-failure
```

Expected: all `Parameters.*` tests PASS.

- [x] **Step 6: Commit**

```bash
git add include/halcyon/parameters.hpp tests/CMakeLists.txt tests/unit/test_parameters.cpp
git commit -m "feat: add anonymous param packing and named ':name' binding"
```

---

## Task 4: `Row` and `ResultSet` (tuple mapping + iteration)

This task introduces `connection.hpp` with `Row` and `ResultSet` only. `Statement`
and `Connection` follow in Task 5 (same header, appended).

**Files:**
- Create: `include/halcyon/connection.hpp` (Row + ResultSet portion)
- Modify: `tests/CMakeLists.txt` (add `unit/test_result_set.cpp`)
- Test: `tests/unit/test_result_set.cpp`

- [x] **Step 1: Write the failing test**

Create `tests/unit/test_result_set.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "halcyon/connection.hpp"
#include "mock_cli_driver.hpp"

using halcyon::ResultSet;
using halcyon::detail::cli::Null;
using halcyon::detail::cli::Value;
using halcyon::testing::MockCliDriver;

namespace {
MockCliDriver::ScriptedRows usersGrid() {
    return MockCliDriver::ScriptedRows{
        {"id", "name"},
        {{Value{std::int64_t{1}}, Value{std::string{"ada"}}},
         {Value{std::int64_t{2}}, Value{std::string{"bob"}}}}};
}
}  // namespace

TEST(ResultSetTest, RangeIterationYieldsTuples) {
    MockCliDriver driver;
    auto conn = driver.connect({"x"}).value();
    driver.resultSets.push_back(usersGrid());
    auto stmt = driver.prepare(conn, "SELECT id, name FROM u").value();
    ASSERT_TRUE(driver.execute(stmt).ok());

    auto rs = ResultSet::create_borrowing(driver, stmt).value();
    EXPECT_EQ(rs.column_count(), 2u);

    std::vector<std::pair<int, std::string>> got;
    for (auto& row : rs) {
        auto [id, name] = row.as<int, std::string>();
        got.emplace_back(id, name);
    }
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].first, 1);
    EXPECT_EQ(got[0].second, "ada");
    EXPECT_EQ(got[1].second, "bob");
}

TEST(ResultSetTest, TryAsReturnsErrorOnArityMismatch) {
    MockCliDriver driver;
    auto conn = driver.connect({"x"}).value();
    driver.resultSets.push_back(usersGrid());
    auto stmt = driver.prepare(conn, "SELECT id, name FROM u").value();
    ASSERT_TRUE(driver.execute(stmt).ok());
    auto rs = ResultSet::create_borrowing(driver, stmt).value();

    auto it = rs.begin();
    ASSERT_NE(it, rs.end());
    auto bad = it->try_as<int>();  // 1 type vs 2 columns
    ASSERT_FALSE(bad.ok());
    EXPECT_EQ(bad.error().code, halcyon::ErrorCode::Mapping);
}

TEST(ResultSetTest, AsThrowsMappingExceptionOnNullIntoNonOptional) {
    MockCliDriver driver;
    auto conn = driver.connect({"x"}).value();
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"name"}, {{Value{Null{}}}}});
    auto stmt = driver.prepare(conn, "SELECT name FROM u").value();
    ASSERT_TRUE(driver.execute(stmt).ok());
    auto rs = ResultSet::create_borrowing(driver, stmt).value();

    auto it = rs.begin();
    ASSERT_NE(it, rs.end());
    EXPECT_THROW(it->as<std::string>(), halcyon::MappingException);
}
```

- [x] **Step 2: Run test to verify it fails**

Add `unit/test_result_set.cpp` to `tests/CMakeLists.txt`, reconfigure, build.
Expected: compile FAIL — `halcyon/connection.hpp` not found.

- [x] **Step 3: Write `connection.hpp` (Row + ResultSet)**

Create `include/halcyon/connection.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <iterator>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/error.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/result.hpp"
#include "halcyon/types.hpp"

namespace halcyon {

// A non-owning view of the current cursor row. Valid only at the iterator's
// current position (forward-only); reads columns lazily via the seam.
class Row {
public:
    Row(detail::cli::ICliDriver& driver, detail::cli::StatementHandle stmt,
        std::size_t columns)
        : driver_(&driver), stmt_(stmt), columns_(columns) {}

    std::size_t column_count() const noexcept { return columns_; }

    // Result form: maps the row to a tuple<Ts...>; Mapping error on arity or
    // type/null mismatch.
    template <class... Ts>
    Result<std::tuple<Ts...>> try_as() const {
        static_assert((is_readable<Ts>::value && ...),
                      "every column type must have a readable TypeBinder");
        if (sizeof...(Ts) != columns_)
            return detail::mapping_error("row.as<>: column count mismatch");
        return read_tuple<Ts...>(std::index_sequence_for<Ts...>{});
    }

    // Throwing form (OO style): unwraps try_as via Result::value().
    template <class... Ts>
    std::tuple<Ts...> as() const {
        return try_as<Ts...>().value();
    }

private:
    template <class... Ts, std::size_t... I>
    Result<std::tuple<Ts...>> read_tuple(std::index_sequence<I...>) const {
        std::tuple<Ts...> out;
        Error err;
        bool ok = true;
        auto step = [&](auto idx, auto* slot) {
            if (!ok) return;
            constexpr std::size_t i = decltype(idx)::value;
            auto cell = driver_->getColumn(stmt_, i);
            if (!cell.ok()) { ok = false; err = cell.error(); return; }
            using F = std::remove_pointer_t<decltype(slot)>;
            auto v = TypeBinder<F>::from_value(cell.value());
            if (!v.ok()) { ok = false; err = v.error(); return; }
            *slot = std::move(v.value());
        };
        (step(std::integral_constant<std::size_t, I>{}, &std::get<I>(out)), ...);
        if (!ok) return err;
        return out;
    }

    detail::cli::ICliDriver* driver_;
    detail::cli::StatementHandle stmt_;
    std::size_t columns_;
};

// A forward-only result cursor over a statement. May borrow the statement
// (Statement keeps ownership) or own it (created by Connection::query). Single
// active cursor per statement; do not outlive the owning Statement when borrowing.
class ResultSet {
public:
    ResultSet(ResultSet&&) = default;
    ResultSet& operator=(ResultSet&&) = default;
    ResultSet(const ResultSet&) = delete;
    ResultSet& operator=(const ResultSet&) = delete;

    static Result<ResultSet> create_borrowing(detail::cli::ICliDriver& driver,
                                              detail::cli::StatementHandle stmt) {
        auto cc = driver.columnCount(stmt);
        if (!cc.ok()) return cc.error();
        return ResultSet(&driver, stmt, cc.value());
    }

    std::size_t column_count() const noexcept { return columns_; }

    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = Row;
        using difference_type = std::ptrdiff_t;
        using pointer = const Row*;
        using reference = const Row&;

        iterator() : rs_(nullptr), at_end_(true), row_(make_invalid_row()) {}
        explicit iterator(ResultSet* rs)
            : rs_(rs), at_end_(false), row_(make_invalid_row()) {
            advance();
        }

        const Row& operator*() const { return row_; }
        const Row* operator->() const { return &row_; }

        iterator& operator++() {
            advance();
            return *this;
        }

        bool operator==(const iterator& o) const noexcept {
            return at_end_ == o.at_end_;
        }
        bool operator!=(const iterator& o) const noexcept {
            return !(*this == o);
        }

    private:
        static Row make_invalid_row() {
            return Row(dummy_driver(), detail::cli::StatementHandle::invalid, 0);
        }
        static detail::cli::ICliDriver& dummy_driver();  // never called

        void advance() {
            auto f = rs_->driver_->fetch(rs_->stmt_);
            if (!f.ok() || !f.value()) {
                at_end_ = true;
                return;
            }
            row_ = Row(*rs_->driver_, rs_->stmt_, rs_->columns_);
        }

        ResultSet* rs_;
        bool at_end_;
        Row row_;
    };

    iterator begin() { return iterator(this); }
    iterator end() { return iterator(); }

private:
    ResultSet(detail::cli::ICliDriver* driver, detail::cli::StatementHandle stmt,
              std::size_t columns)
        : driver_(driver), stmt_(stmt), columns_(columns) {}

    friend class Connection;  // for the owning-ResultSet constructor in Task 5
    detail::cli::ICliDriver* driver_;
    detail::cli::StatementHandle stmt_;
    std::size_t columns_;
};

}  // namespace halcyon
```

> Implementation note: `iterator` holds a `Row` by value but `Row` has no default
> constructor, so the iterator seeds it via `make_invalid_row()`. `dummy_driver()`
> is declared but never defined and never called (the invalid row is overwritten
> by `advance()` before any dereference, and an `end()` iterator is never
> dereferenced). If a linker complains, replace the `Row row_;` member with
> `std::optional<Row>` and adjust `operator*`/`advance()` accordingly.

- [x] **Step 4: Adopt the `std::optional<Row>` iterator (robust variant)**

To avoid the undefined `dummy_driver()` entirely, implement the iterator member
as `std::optional<Row>`:

```cpp
    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = Row;
        using difference_type = std::ptrdiff_t;
        using pointer = const Row*;
        using reference = const Row&;

        iterator() = default;  // end sentinel: at_end_ == true
        explicit iterator(ResultSet* rs) : rs_(rs), at_end_(false) { advance(); }

        const Row& operator*() const { return *row_; }
        const Row* operator->() const { return &*row_; }
        iterator& operator++() { advance(); return *this; }
        bool operator==(const iterator& o) const noexcept { return at_end_ == o.at_end_; }
        bool operator!=(const iterator& o) const noexcept { return !(*this == o); }

    private:
        void advance() {
            auto f = rs_->driver_->fetch(rs_->stmt_);
            if (!f.ok() || !f.value()) { at_end_ = true; row_.reset(); return; }
            row_.emplace(*rs_->driver_, rs_->stmt_, rs_->columns_);
        }
        ResultSet* rs_ = nullptr;
        bool at_end_ = true;
        std::optional<Row> row_;
    };
```

Use this version (delete the `make_invalid_row()`/`dummy_driver()` version from
Step 3). The Step-3 listing documents the design; this is the code to keep.

- [x] **Step 5: Wire into the build**

Add `unit/test_result_set.cpp` to `halcyon_unit_tests` in `tests/CMakeLists.txt`.

- [x] **Step 6: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R ResultSetTest --output-on-failure
```

Expected: all `ResultSetTest.*` tests PASS.

- [x] **Step 7: Commit**

```bash
git add include/halcyon/connection.hpp tests/CMakeLists.txt tests/unit/test_result_set.cpp
git commit -m "feat: add Row and forward-only ResultSet with tuple mapping"
```

---

## Task 5: `Statement` and `Connection` (`query` / `execute`)

**Files:**
- Modify: `include/halcyon/connection.hpp` (append `Statement` + `Connection`)
- Modify: `tests/CMakeLists.txt` (add `unit/test_connection.cpp`)
- Test: `tests/unit/test_connection.cpp`

- [x] **Step 1: Write the failing test**

Create `tests/unit/test_connection.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "halcyon/connection.hpp"
#include "halcyon/parameters.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Connection;
using halcyon::params;
using halcyon::detail::cli::Value;
using halcyon::testing::MockCliDriver;

TEST(ConnectionTest, OpenConnectsAndDisconnectsOnDestruction) {
    MockCliDriver driver;
    {
        auto c = Connection::open(driver, {"DATABASE=SAMPLE;"});
        ASSERT_TRUE(c.ok());
        EXPECT_EQ(driver.connectCalls, 1);
    }
    EXPECT_EQ(driver.disconnectCalls, 1);
}

TEST(ConnectionTest, ExecuteBindsAnonymousParamsAndReturnsRowCount) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(2);
    auto c = Connection::open(driver, {"x"}).value();

    auto n = c.execute("UPDATE t SET a = ? WHERE b = ?", 9, std::string{"k"});
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 2);
    ASSERT_FALSE(driver.preparedSql.empty());
    EXPECT_EQ(driver.preparedSql.back(), "UPDATE t SET a = ? WHERE b = ?");
    // the prepared statement's bound params were [9, "k"]
    auto& st = driver.statements.begin()->second;
    ASSERT_EQ(st.boundParams.size(), 2u);
    EXPECT_EQ(st.boundParams[0], Value{std::int64_t{9}});
}

TEST(ConnectionTest, QueryAnonymousReturnsIterableResultSet) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"}, {{Value{std::int64_t{10}}}, {Value{std::int64_t{20}}}}});
    auto c = Connection::open(driver, {"x"}).value();

    auto rs = c.query("SELECT id FROM t WHERE id > ?", 5);
    ASSERT_TRUE(rs.ok());
    long sum = 0;
    for (auto& row : rs.value()) sum += std::get<0>(row.as<int>());
    EXPECT_EQ(sum, 30);
}

TEST(ConnectionTest, QueryNamedRewritesPlaceholders) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{{"id"}, {}});
    auto c = Connection::open(driver, {"x"}).value();

    auto rs = c.query("SELECT id FROM t WHERE age > :age",
                      params{{"age", 21}});
    ASSERT_TRUE(rs.ok());
    EXPECT_EQ(driver.preparedSql.back(), "SELECT id FROM t WHERE age > ?");
    auto& st = driver.statements.begin()->second;
    ASSERT_EQ(st.boundParams.size(), 1u);
    EXPECT_EQ(st.boundParams[0], Value{std::int64_t{21}});
}

TEST(ConnectionTest, PreparedStatementIsReusable) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(1);
    driver.execRowCounts.push_back(1);
    auto c = Connection::open(driver, {"x"}).value();

    auto st = c.prepare("INSERT INTO t(a) VALUES (?)");
    ASSERT_TRUE(st.ok());
    EXPECT_EQ(st.value().execute_update(1).value(), 1);
    EXPECT_EQ(st.value().execute_update(2).value(), 1);
    EXPECT_EQ(driver.preparedSql.size(), 1u);  // prepared once, executed twice
}
```

- [x] **Step 2: Run test to verify it fails**

Add `unit/test_connection.cpp` to `tests/CMakeLists.txt`, reconfigure, build.
Expected: compile FAIL — `Statement`/`Connection` undefined.

- [x] **Step 3: Append `Statement` and `Connection` to `connection.hpp`**

Insert these classes inside `namespace halcyon` in
`include/halcyon/connection.hpp`, *before* the closing brace (after `ResultSet`):

```cpp
// A prepared, reusable statement. Owns its driver statement handle and finalizes
// it on destruction. Move-only.
class Statement {
public:
    Statement(detail::cli::ICliDriver& driver, detail::cli::StatementHandle handle)
        : driver_(&driver), handle_(handle) {}

    Statement(Statement&& o) noexcept
        : driver_(o.driver_), handle_(o.handle_) {
        o.handle_ = detail::cli::StatementHandle::invalid;
    }
    Statement& operator=(Statement&& o) noexcept {
        if (this != &o) {
            reset();
            driver_ = o.driver_;
            handle_ = o.handle_;
            o.handle_ = detail::cli::StatementHandle::invalid;
        }
        return *this;
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement() { reset(); }

    detail::cli::StatementHandle handle() const noexcept { return handle_; }

    // Binds params, executes, and returns a borrowing ResultSet (valid until the
    // next exec on this statement or until *this is destroyed).
    template <class... Args>
    Result<ResultSet> execute_query(const Args&... args) {
        auto pre = exec(detail::pack_params(args...));
        if (!pre.ok()) return pre.error();
        return ResultSet::create_borrowing(*driver_, handle_);
    }

    template <class... Args>
    Result<std::int64_t> execute_update(const Args&... args) {
        return exec(detail::pack_params(args...));
    }

private:
    Result<std::int64_t> exec(const std::vector<detail::cli::Value>& params) {
        auto b = driver_->bindParams(handle_, params);
        if (!b.ok()) return b.error();
        return driver_->execute(handle_);
    }
    void reset() {
        if (driver_ && handle_ != detail::cli::StatementHandle::invalid) {
            driver_->finalize(handle_);
            handle_ = detail::cli::StatementHandle::invalid;
        }
    }
    detail::cli::ICliDriver* driver_;
    detail::cli::StatementHandle handle_;
};

// A single logical connection over the seam. Owns the physical connection handle
// and disconnects on destruction. Move-only; not safe for concurrent use by
// multiple threads (matches CLI handle semantics).
class Connection {
public:
    static Result<Connection> open(detail::cli::ICliDriver& driver,
                                   const detail::cli::ConnectionParams& params) {
        auto h = driver.connect(params);
        if (!h.ok()) return h.error();
        return Connection(driver, h.value());
    }

    Connection(detail::cli::ICliDriver& driver, detail::cli::ConnectionHandle handle)
        : driver_(&driver), handle_(handle) {}

    Connection(Connection&& o) noexcept : driver_(o.driver_), handle_(o.handle_) {
        o.handle_ = detail::cli::ConnectionHandle::invalid;
    }
    Connection& operator=(Connection&& o) noexcept {
        if (this != &o) {
            reset();
            driver_ = o.driver_;
            handle_ = o.handle_;
            o.handle_ = detail::cli::ConnectionHandle::invalid;
        }
        return *this;
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection() { reset(); }

    detail::cli::ConnectionHandle handle() const noexcept { return handle_; }

    Result<Statement> prepare(const std::string& sql) {
        auto h = driver_->prepare(handle_, sql);
        if (!h.ok()) return h.error();
        return Statement(*driver_, h.value());
    }

    // --- anonymous-parameter overloads (enabled only when all Args bind) ---
    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<ResultSet> query(const std::string& sql, const Args&... args) {
        auto st = prepare(sql);
        if (!st.ok()) return st.error();
        return run_query(std::move(st.value()), detail::pack_params(args...));
    }

    template <class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::int64_t> execute(const std::string& sql, const Args&... args) {
        auto st = prepare(sql);
        if (!st.ok()) return st.error();
        return st.value().execute_update(args...);
    }

    // --- named-parameter overloads ---
    Result<ResultSet> query(const std::string& sql, const params& named) {
        auto pre = detail::bind_named(sql, named);
        if (!pre.ok()) return pre.error();
        auto st = prepare(pre.value().sql);
        if (!st.ok()) return st.error();
        return run_query(std::move(st.value()), pre.value().params);
    }

    Result<std::int64_t> execute(const std::string& sql, const params& named) {
        auto pre = detail::bind_named(sql, named);
        if (!pre.ok()) return pre.error();
        auto st = prepare(pre.value().sql);
        if (!st.ok()) return st.error();
        auto b = driver_->bindParams(st.value().handle(), pre.value().params);
        if (!b.ok()) return b.error();
        return driver_->execute(st.value().handle());
    }

private:
    // Builds a ResultSet that OWNS the statement (kept alive for the cursor).
    Result<ResultSet> run_query(Statement&& st,
                                const std::vector<detail::cli::Value>& params) {
        auto b = driver_->bindParams(st.handle(), params);
        if (!b.ok()) return b.error();
        auto e = driver_->execute(st.handle());
        if (!e.ok()) return e.error();
        auto cc = driver_->columnCount(st.handle());
        if (!cc.ok()) return cc.error();
        ResultSet rs(driver_, st.handle(), cc.value());
        rs.owned_ = std::move(st);
        return rs;
    }

    void reset() {
        if (driver_ && handle_ != detail::cli::ConnectionHandle::invalid) {
            driver_->disconnect(handle_);
            handle_ = detail::cli::ConnectionHandle::invalid;
        }
    }
    detail::cli::ICliDriver* driver_;
    detail::cli::ConnectionHandle handle_;
};
```

- [x] **Step 4: Give `ResultSet` optional ownership of a `Statement`**

`run_query` sets `rs.owned_`, so `ResultSet` needs that member. In `connection.hpp`,
because `Statement` is defined *after* `ResultSet`, move `Statement`'s definition
*above* `ResultSet`, then add to `ResultSet`'s private section:

```cpp
    std::optional<Statement> owned_;  // present when the ResultSet owns its statement
```

Resulting order in `connection.hpp`: `Row` → `Statement` → `ResultSet` →
`Connection`. (`Statement` does not depend on `ResultSet`'s definition for its own
class body since `execute_query` returns `Result<ResultSet>` — only a declaration
is needed there; forward-declare `class ResultSet;` near the top of the namespace,
and keep `Statement::execute_query`'s body after `ResultSet` is complete by
defining it out-of-line, or simply place `Statement` after `ResultSet` and give
`ResultSet` a forward-declared `std::optional<Statement>`.)

To keep it simple and avoid ordering pitfalls, use this concrete arrangement:

1. `class ResultSet;` and `class Statement;` forward declarations at the top.
2. Define `Row` (unchanged).
3. Define `ResultSet` with `std::optional<Statement> owned_;` (the `<optional>` and
   `Statement` only need to be *complete* where `ResultSet`'s special members are
   instantiated — which is in the `.cpp`/test TUs after both are defined, so a
   forward declaration suffices in the class body for a `std::optional` member only
   if `Statement` is complete at instantiation; therefore define `Statement` BEFORE
   `ResultSet`).
4. Define `Statement` with `execute_query` declared inline but its body referring
   to `ResultSet::create_borrowing` — define that body *after* `ResultSet` (out of
   class) to satisfy completeness.

Final, unambiguous layout to write:

```cpp
namespace halcyon {
class ResultSet;  // fwd

class Row { /* as in Task 4 */ };

class Statement {
    // ... members as above, but declare execute_query without a body:
    template <class... Args> Result<ResultSet> execute_query(const Args&... args);
    template <class... Args> Result<std::int64_t> execute_update(const Args&... args) {
        return exec(detail::pack_params(args...));
    }
    // ... rest unchanged ...
};

class ResultSet {
    // ... as in Task 4, plus:
    std::optional<Statement> owned_;
    friend class Connection;
};

// out-of-line definition now that ResultSet is complete:
template <class... Args>
Result<ResultSet> Statement::execute_query(const Args&... args) {
    auto pre = exec(detail::pack_params(args...));
    if (!pre.ok()) return pre.error();
    return ResultSet::create_borrowing(*driver_, handle_);
}

class Connection { /* as above */ };
}  // namespace halcyon
```

- [x] **Step 5: Wire into the build**

Add `unit/test_connection.cpp` to `halcyon_unit_tests` in `tests/CMakeLists.txt`.

- [x] **Step 6: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R ConnectionTest --output-on-failure
```

Expected: all `ConnectionTest.*` tests PASS.

- [x] **Step 7: Commit**

```bash
git add include/halcyon/connection.hpp tests/CMakeLists.txt tests/unit/test_connection.cpp
git commit -m "feat: add Connection and reusable Statement query/execute over the seam"
```

---

## Task 6: `HALCYON_REFLECT` + `queryAs<T>`

**Files:**
- Modify: `include/halcyon/types.hpp` (reflection machinery + macro)
- Modify: `include/halcyon/connection.hpp` (`Connection::queryAs<T>`)
- Modify: `tests/CMakeLists.txt` (add `unit/test_reflect.cpp`)
- Test: `tests/unit/test_reflect.cpp`

- [x] **Step 1: Write the failing test**

Create `tests/unit/test_reflect.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "halcyon/connection.hpp"
#include "halcyon/types.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Connection;
using halcyon::detail::cli::Null;
using halcyon::detail::cli::Value;
using halcyon::testing::MockCliDriver;

struct User {
    int id;
    std::string name;
    std::optional<int> age;
};
HALCYON_REFLECT(User, id, name, age);

TEST(Reflect, FieldCountAndFlag) {
    static_assert(halcyon::reflect::Reflected<User>::value);
    EXPECT_EQ(halcyon::reflect::Reflected<User>::field_count, 3u);
}

TEST(Reflect, QueryAsMapsRowsToStructs) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id", "name", "age"},
        {{Value{std::int64_t{1}}, Value{std::string{"ada"}}, Value{std::int64_t{36}}},
         {Value{std::int64_t{2}}, Value{std::string{"bob"}}, Value{Null{}}}}});
    auto c = Connection::open(driver, {"x"}).value();

    auto users = c.queryAs<User>("SELECT id, name, age FROM users");
    ASSERT_TRUE(users.ok());
    ASSERT_EQ(users.value().size(), 2u);
    EXPECT_EQ(users.value()[0].id, 1);
    EXPECT_EQ(users.value()[0].name, "ada");
    ASSERT_TRUE(users.value()[0].age.has_value());
    EXPECT_EQ(*users.value()[0].age, 36);
    EXPECT_FALSE(users.value()[1].age.has_value());  // NULL -> nullopt
}

TEST(Reflect, QueryAsArityMismatchIsMappingError) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id", "name"},
        {{Value{std::int64_t{1}}, Value{std::string{"ada"}}}}});  // 2 cols, 3 fields
    auto c = Connection::open(driver, {"x"}).value();

    auto users = c.queryAs<User>("SELECT id, name FROM users");
    ASSERT_FALSE(users.ok());
    EXPECT_EQ(users.error().code, halcyon::ErrorCode::Mapping);
}
```

- [x] **Step 2: Run test to verify it fails**

Add `unit/test_reflect.cpp` to `tests/CMakeLists.txt`, reconfigure, build.
Expected: compile FAIL — `HALCYON_REFLECT`/`queryAs` undefined.

- [x] **Step 3: Add reflection machinery to `types.hpp`**

Append to `include/halcyon/types.hpp`, inside `namespace halcyon` add a
`reflect` namespace, then define the macros at global scope (outside any
namespace) at the very end of the file:

```cpp
namespace halcyon {
namespace reflect {

// Primary: a type is not reflected unless HALCYON_REFLECT specializes this.
template <class T>
struct Reflected : std::false_type {
    static constexpr std::size_t field_count = 0;
};

// Assigns each column (by position) into the struct via its member pointers.
template <class T, class Tuple, std::size_t... I>
Result<void> assign_fields(T& out, const Tuple& mptrs,
                           detail::cli::ICliDriver& driver,
                           detail::cli::StatementHandle stmt,
                           std::index_sequence<I...>) {
    Error err;
    bool ok = true;
    auto step = [&](auto idx) {
        if (!ok) return;
        constexpr std::size_t i = decltype(idx)::value;
        auto mp = std::get<i>(mptrs);
        using F = std::remove_reference_t<decltype(out.*mp)>;
        auto cell = driver.getColumn(stmt, i);
        if (!cell.ok()) { ok = false; err = cell.error(); return; }
        auto v = TypeBinder<F>::from_value(cell.value());
        if (!v.ok()) { ok = false; err = v.error(); return; }
        out.*mp = std::move(v.value());
    };
    (step(std::integral_constant<std::size_t, I>{}), ...);
    if (!ok) return err;
    return Result<void>();
}

// Maps the current cursor row into a freshly value-initialized T.
template <class T>
Result<T> map_row(detail::cli::ICliDriver& driver,
                  detail::cli::StatementHandle stmt, std::size_t columns) {
    static_assert(Reflected<T>::value,
                  "queryAs<T> requires HALCYON_REFLECT(T, fields...)");
    constexpr std::size_t N = Reflected<T>::field_count;
    if (columns != N)
        return detail::mapping_error("queryAs<T>: column count != field count");
    auto mptrs = Reflected<T>::members();
    T out{};
    auto r = assign_fields(out, mptrs, driver, stmt, std::make_index_sequence<N>{});
    if (!r.ok()) return r.error();
    return out;
}

}  // namespace reflect
}  // namespace halcyon

// ---- HALCYON_REFLECT macro (global scope) ----
// Expands field names to a tuple of member pointers and specializes Reflected<T>.
#define HALCYON_PP_EXPAND(x) x
#define HALCYON_PP_CAT(a, b) HALCYON_PP_CAT_(a, b)
#define HALCYON_PP_CAT_(a, b) a##b

#define HALCYON_PP_NARG(...) \
    HALCYON_PP_EXPAND(HALCYON_PP_NARG_(__VA_ARGS__, HALCYON_PP_RSEQ()))
#define HALCYON_PP_NARG_(...) HALCYON_PP_EXPAND(HALCYON_PP_ARG_N(__VA_ARGS__))
#define HALCYON_PP_ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, N, ...) N
#define HALCYON_PP_RSEQ() 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0

#define HALCYON_MP(T, field) &T::field
#define HALCYON_FE_1(T, a) HALCYON_MP(T, a)
#define HALCYON_FE_2(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_1(T, __VA_ARGS__))
#define HALCYON_FE_3(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_2(T, __VA_ARGS__))
#define HALCYON_FE_4(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_3(T, __VA_ARGS__))
#define HALCYON_FE_5(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_4(T, __VA_ARGS__))
#define HALCYON_FE_6(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_5(T, __VA_ARGS__))
#define HALCYON_FE_7(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_6(T, __VA_ARGS__))
#define HALCYON_FE_8(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_7(T, __VA_ARGS__))
#define HALCYON_FE_9(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_8(T, __VA_ARGS__))
#define HALCYON_FE_10(T, a, ...) HALCYON_MP(T, a), HALCYON_PP_EXPAND(HALCYON_FE_9(T, __VA_ARGS__))
#define HALCYON_MEMBER_PTRS(T, ...) \
    HALCYON_PP_EXPAND(HALCYON_PP_CAT(HALCYON_FE_, HALCYON_PP_NARG(__VA_ARGS__))(T, __VA_ARGS__))

// Supports up to 10 fields; extend the HALCYON_FE_<n>/HALCYON_PP_* ladder for more.
#define HALCYON_REFLECT(Type, ...)                                          \
    template <>                                                             \
    struct halcyon::reflect::Reflected<Type> : std::true_type {             \
        static constexpr std::size_t field_count = HALCYON_PP_NARG(__VA_ARGS__); \
        static auto members() {                                             \
            return std::make_tuple(HALCYON_MEMBER_PTRS(Type, __VA_ARGS__)); \
        }                                                                   \
    }
```

> Note: `Reflected<T>` inherits `std::true_type` only in the specialization, so
> `Reflected<T>::value` is `true` for reflected types and `false` otherwise. Add
> `#include <tuple>` to `types.hpp` (for `std::make_tuple`/`std::get`).

- [x] **Step 4: Add `Connection::queryAs<T>`**

In `include/halcyon/connection.hpp`, add to `Connection` (anonymous + named):

```cpp
    template <class T, class... Args,
              std::enable_if_t<(is_bindable<Args>::value && ...), int> = 0>
    Result<std::vector<T>> queryAs(const std::string& sql, const Args&... args) {
        auto st = prepare(sql);
        if (!st.ok()) return st.error();
        return collect<T>(st.value(), detail::pack_params(args...));
    }

    template <class T>
    Result<std::vector<T>> queryAs(const std::string& sql, const params& named) {
        auto pre = detail::bind_named(sql, named);
        if (!pre.ok()) return pre.error();
        auto st = prepare(pre.value().sql);
        if (!st.ok()) return st.error();
        return collect<T>(st.value(), pre.value().params);
    }

private:
    template <class T>
    Result<std::vector<T>> collect(Statement& st,
                                   const std::vector<detail::cli::Value>& params) {
        auto b = driver_->bindParams(st.handle(), params);
        if (!b.ok()) return b.error();
        auto e = driver_->execute(st.handle());
        if (!e.ok()) return e.error();
        auto cc = driver_->columnCount(st.handle());
        if (!cc.ok()) return cc.error();
        std::vector<T> out;
        for (;;) {
            auto f = driver_->fetch(st.handle());
            if (!f.ok()) return f.error();
            if (!f.value()) break;
            auto row = reflect::map_row<T>(*driver_, st.handle(), cc.value());
            if (!row.ok()) return row.error();
            out.push_back(std::move(row.value()));
        }
        return out;
    }
```

(Place the existing `private:` section accordingly — there should be a single
`private:` block; merge `collect` and `run_query`/`reset`/members.)

- [x] **Step 5: Wire into the build**

Add `unit/test_reflect.cpp` to `halcyon_unit_tests` in `tests/CMakeLists.txt`.

- [x] **Step 6: Build and run to verify pass**

```bash
cmake --build build -j
ctest --test-dir build -R Reflect --output-on-failure
```

Expected: all `Reflect.*` tests PASS.

- [x] **Step 7: Commit**

```bash
git add include/halcyon/types.hpp include/halcyon/connection.hpp \
        tests/CMakeLists.txt tests/unit/test_reflect.cpp
git commit -m "feat: add HALCYON_REFLECT and queryAs<T> struct mapping"
```

---

## Task 7: SQLSTATE → `ErrorCode` classification (portable, unit-tested)

This is the only piece of the driver that is *pure* C++ and must be tested
without a live DB; it is extracted so Task 8 just calls it.

**Files:**
- Create: `include/halcyon/detail/cli/sqlstate.hpp`
- Create: `src/detail/cli/sqlstate.cpp`
- Modify: `CMakeLists.txt` (add `src/detail/cli/sqlstate.cpp` to the library)
- Modify: `tests/CMakeLists.txt` (add `unit/test_sqlstate.cpp`)
- Test: `tests/unit/test_sqlstate.cpp`

- [x] **Step 1: Write the failing test**

Create `tests/unit/test_sqlstate.cpp`:

```cpp
#include <gtest/gtest.h>

#include "halcyon/detail/cli/sqlstate.hpp"

using halcyon::ErrorCode;
using halcyon::detail::cli::classify_sqlstate;

TEST(SqlState, ConnectionClassIsRetriable) {
    auto c = classify_sqlstate("08001", 0);
    EXPECT_EQ(c.code, ErrorCode::Connection);
    EXPECT_TRUE(c.retriable);
}

TEST(SqlState, CommLostNativeCodeIsTransient) {
    auto c = classify_sqlstate("", -30081);
    EXPECT_EQ(c.code, ErrorCode::Transient);
    EXPECT_TRUE(c.retriable);
}

TEST(SqlState, DeadlockAndLockTimeout) {
    EXPECT_EQ(classify_sqlstate("40001", 0).code, ErrorCode::Deadlock);
    EXPECT_TRUE(classify_sqlstate("40001", 0).retriable);
    EXPECT_EQ(classify_sqlstate("57033", 0).code, ErrorCode::Timeout);
    EXPECT_TRUE(classify_sqlstate("57033", 0).retriable);
}

TEST(SqlState, ConstraintAndSyntaxAreNotRetriable) {
    EXPECT_EQ(classify_sqlstate("23505", 0).code, ErrorCode::Constraint);
    EXPECT_FALSE(classify_sqlstate("23505", 0).retriable);
    EXPECT_EQ(classify_sqlstate("42601", 0).code, ErrorCode::Syntax);
    EXPECT_FALSE(classify_sqlstate("42601", 0).retriable);
}

TEST(SqlState, UnknownDefault) {
    auto c = classify_sqlstate("99999", 0);
    EXPECT_EQ(c.code, ErrorCode::Unknown);
    EXPECT_FALSE(c.retriable);
}
```

- [x] **Step 2: Write the classification header**

Create `include/halcyon/detail/cli/sqlstate.hpp`:

```cpp
#pragma once

#include <string_view>

#include "halcyon/error.hpp"

namespace halcyon::detail::cli {

struct Classification {
    ErrorCode code = ErrorCode::Unknown;
    bool retriable = false;
};

// Maps a Db2 SQLSTATE (and native SQLCODE) to a Halcyon ErrorCode per the spec.
Classification classify_sqlstate(std::string_view sqlstate, int nativeError) noexcept;

}  // namespace halcyon::detail::cli
```

- [x] **Step 3: Write the classification source**

Create `src/detail/cli/sqlstate.cpp`:

```cpp
#include "halcyon/detail/cli/sqlstate.hpp"

namespace halcyon::detail::cli {

namespace {
bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}
}  // namespace

Classification classify_sqlstate(std::string_view sqlstate,
                                 int nativeError) noexcept {
    if (starts_with(sqlstate, "08")) return {ErrorCode::Connection, true};
    if (nativeError == -30081) return {ErrorCode::Transient, true};
    if (sqlstate == "40001") return {ErrorCode::Deadlock, true};
    if (sqlstate == "57033" || starts_with(sqlstate, "57")) {
        // 57033 lock timeout / resource-not-available class: treat as retriable
        // timeout.
        return {ErrorCode::Timeout, true};
    }
    if (starts_with(sqlstate, "23")) return {ErrorCode::Constraint, false};
    if (starts_with(sqlstate, "42")) return {ErrorCode::Syntax, false};
    return {ErrorCode::Unknown, false};
}

}  // namespace halcyon::detail::cli
```

- [x] **Step 4: Wire into the build**

In `CMakeLists.txt`, add the source to the library target:

```cmake
add_library(halcyon
    src/core/version.cpp
    src/core/error.cpp
    src/detail/cli/sqlstate.cpp)
```

Add `unit/test_sqlstate.cpp` to `halcyon_unit_tests` in `tests/CMakeLists.txt`.

- [x] **Step 5: Build and run to verify pass**

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build -R SqlState --output-on-failure
```

Expected: all `SqlState.*` tests PASS.

- [x] **Step 6: Commit**

```bash
git add include/halcyon/detail/cli/sqlstate.hpp src/detail/cli/sqlstate.cpp \
        CMakeLists.txt tests/CMakeLists.txt tests/unit/test_sqlstate.cpp
git commit -m "feat: add portable SQLSTATE to ErrorCode classification"
```

---

## Task 8: Real `Db2CliDriver` over `sqlcli1.h`

This is the only translation unit that includes IBM CLI headers. It is verified by
the build (compiles + links `DB2::CLI`) and exercised end-to-end by Task 9's
integration tests; there is no live-DB unit test here.

**Files:**
- Create: `include/halcyon/detail/cli/db2_cli_driver.hpp` (factory; no `sqlcli1.h`)
- Create: `src/detail/cli/db2_cli_driver.cpp`
- Modify: `CMakeLists.txt` (add source, link `DB2::CLI`, bake RPATH)

- [x] **Step 1: Write the factory header (portable)**

Create `include/halcyon/detail/cli/db2_cli_driver.hpp`:

```cpp
#pragma once

#include <memory>

#include "halcyon/detail/cli/driver.hpp"

namespace halcyon::detail::cli {

// Constructs the real IBM Db2 CLI driver. Defined in db2_cli_driver.cpp, the only
// translation unit that includes sqlcli1.h. Callers never see CLI types.
std::unique_ptr<ICliDriver> make_db2_cli_driver();

}  // namespace halcyon::detail::cli
```

- [x] **Step 2: Write the driver implementation**

Create `src/detail/cli/db2_cli_driver.cpp`. Includes both `sqlcli1.h` (core +
`SQLSetConnectAttr`) and `sqlext.h` (`SQLDriverConnect`, `SQLBindParameter`). Do
**not** define `UNICODE` (so the `SQLCHAR*` UTF-8 entry points are used).

```cpp
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

private:
    struct StmtState {
        SQLHSTMT handle;
        std::vector<BoundParam> bound;
    };

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
```

> The exact `SQL_*` constants and length-indicator nuances are validated by the
> Task 9 integration build/run against the real driver (TDD against live Db2).
> Treat `get_string` truncation handling and `BoundParam` lifetimes as the most
> likely spots to adjust when first running against a server.

- [x] **Step 3: Wire the driver into the library and link `DB2::CLI`**

In `CMakeLists.txt`, add the source and link the imported target (and bake the
RPATH per the spec so examples/tests find `libdb2` without `*_LIBRARY_PATH`):

```cmake
add_library(halcyon
    src/core/version.cpp
    src/core/error.cpp
    src/detail/cli/sqlstate.cpp
    src/detail/cli/db2_cli_driver.cpp)

# ... existing target_include_directories / features / warnings ...

target_link_libraries(halcyon PRIVATE DB2::CLI)

if(DB2CLI_LIBRARY_DIR)
    set_target_properties(halcyon PROPERTIES
        BUILD_RPATH "${DB2CLI_LIBRARY_DIR}"
        INSTALL_RPATH "${DB2CLI_LIBRARY_DIR}")
endif()
```

- [x] **Step 4: Build to verify it compiles and links**

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON
cmake --build build -j
```

Expected: `halcyon` compiles `db2_cli_driver.cpp` against `sqlcli1.h`/`sqlext.h`
and links `DB2::CLI`. Unit tests still build and pass (they use the mock; the real
driver is not invoked):

```bash
ctest --test-dir build --output-on-failure
```

- [x] **Step 5: Commit**

```bash
git add include/halcyon/detail/cli/db2_cli_driver.hpp \
        src/detail/cli/db2_cli_driver.cpp CMakeLists.txt
git commit -m "feat: implement real Db2CliDriver over sqlcli1.h and link DB2::CLI"
```

---

## Task 9: Dockerized Db2 integration tests + umbrella header

**Files:**
- Create: `include/halcyon/halcyon.hpp`
- Create: `docker/docker-compose.yml`
- Create: `tests/integration/CMakeLists.txt`
- Create: `tests/integration/test_db2_roundtrip.cpp`
- Modify: `CMakeLists.txt` (add `HALCYON_BUILD_INTEGRATION_TESTS` option)
- Modify: `tests/CMakeLists.txt` (add the integration subdir when enabled)

- [x] **Step 1: Write the umbrella header**

Create `include/halcyon/halcyon.hpp`:

```cpp
#pragma once

// Halcyon — modern C++17 IBM Db2 client. Umbrella header.
#include "halcyon/connection.hpp"
#include "halcyon/error.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/result.hpp"
#include "halcyon/types.hpp"
#include "halcyon/version.hpp"
#include "halcyon/detail/cli/db2_cli_driver.hpp"
```

- [x] **Step 2: Write the Docker compose for Db2**

Create `docker/docker-compose.yml`:

```yaml
services:
  db2:
    image: icr.io/db2_community/db2:11.5.9.0
    privileged: true
    environment:
      LICENSE: accept
      DB2INST1_PASSWORD: halcyon
      DBNAME: SAMPLE
      PERSISTENT_HOME: "false"
    ports:
      - "50000:50000"
    healthcheck:
      test: ["CMD-SHELL", "su - db2inst1 -c 'db2 connect to SAMPLE' || exit 1"]
      interval: 20s
      timeout: 10s
      retries: 30
```

- [x] **Step 3: Write the integration test**

Create `tests/integration/test_db2_roundtrip.cpp`. It is skipped (passes trivially)
unless `HALCYON_TEST_DSN` is set, so the target can be built everywhere but only
runs against a live DB.

```cpp
#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>

#include "halcyon/halcyon.hpp"

using halcyon::Connection;
using halcyon::params;

namespace {
std::optional<std::string> dsn() {
    if (const char* v = std::getenv("HALCYON_TEST_DSN")) return std::string(v);
    return std::nullopt;
}
}  // namespace

struct Person {
    int id;
    std::string name;
    std::optional<int> age;
};
HALCYON_REFLECT(Person, id, name, age);

class Db2Integration : public ::testing::Test {
protected:
    void SetUp() override {
        auto d = dsn();
        if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";
        driver_ = halcyon::detail::cli::make_db2_cli_driver();
        auto c = Connection::open(*driver_, {*d});
        ASSERT_TRUE(c.ok()) << c.error().message;
        conn_ = std::make_unique<Connection>(std::move(c.value()));
        conn_->execute("DROP TABLE halcyon_people");  // ignore error if absent
        ASSERT_TRUE(
            conn_->execute("CREATE TABLE halcyon_people("
                           "id INT NOT NULL, name VARCHAR(64), age INT)")
                .ok());
    }

    std::unique_ptr<halcyon::detail::cli::ICliDriver> driver_;
    std::unique_ptr<Connection> conn_;
};

TEST_F(Db2Integration, InsertSelectStructMapping) {
    ASSERT_EQ(conn_->execute("INSERT INTO halcyon_people VALUES (?, ?, ?)", 1,
                             std::string{"ada"}, 36)
                  .value(),
              1);
    ASSERT_TRUE(conn_
                    ->execute("INSERT INTO halcyon_people(id, name) VALUES "
                              "(:id, :name)",
                              params{{"id", 2}, {"name", std::string{"bob"}}})
                    .ok());

    auto people =
        conn_->queryAs<Person>("SELECT id, name, age FROM halcyon_people ORDER BY id");
    ASSERT_TRUE(people.ok()) << people.error().message;
    ASSERT_EQ(people.value().size(), 2u);
    EXPECT_EQ(people.value()[0].name, "ada");
    EXPECT_EQ(*people.value()[0].age, 36);
    EXPECT_FALSE(people.value()[1].age.has_value());
}

TEST_F(Db2Integration, TupleIterationAndAnonymousParams) {
    ASSERT_TRUE(conn_->execute("INSERT INTO halcyon_people VALUES (?,?,?)", 10,
                               std::string{"x"}, 1).ok());
    auto rs = conn_->query("SELECT id, name FROM halcyon_people WHERE id >= ?", 10);
    ASSERT_TRUE(rs.ok()) << rs.error().message;
    int count = 0;
    for (auto& row : rs.value()) {
        auto [id, name] = row.as<int, std::string>();
        EXPECT_GE(id, 10);
        (void)name;
        ++count;
    }
    EXPECT_GE(count, 1);
}
```

- [x] **Step 4: Write the integration CMake**

Create `tests/integration/CMakeLists.txt`:

```cmake
add_executable(halcyon_integration_tests
    test_db2_roundtrip.cpp)

target_link_libraries(halcyon_integration_tests
    PRIVATE halcyon::halcyon GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(halcyon_integration_tests
    PROPERTIES LABELS integration)
```

- [x] **Step 5: Wire the option and subdir**

In `CMakeLists.txt`, add the option near the others:

```cmake
option(HALCYON_BUILD_INTEGRATION_TESTS "Build Dockerized Db2 integration tests" OFF)
```

In `tests/CMakeLists.txt`, after the unit-test setup:

```cmake
if(HALCYON_BUILD_INTEGRATION_TESTS)
    add_subdirectory(integration)
endif()
```

- [x] **Step 6: Build the integration target (no DB required to compile)**

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON -DHALCYON_BUILD_INTEGRATION_TESTS=ON
cmake --build build -j
ctest --test-dir build -L integration --output-on-failure
```

Expected without a DB: tests are reported as **skipped** (no `HALCYON_TEST_DSN`).

- [x] **Step 7: Run the integration suite against live Db2 (optional, gated)**

```bash
docker compose -f docker/docker-compose.yml up -d
# wait for healthy, then:
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
ctest --test-dir build -L integration --output-on-failure
docker compose -f docker/docker-compose.yml down
```

Expected: `Db2Integration.*` PASS against the live database.

- [x] **Step 8: Run the full unit suite**

```bash
ctest --test-dir build --output-on-failure
```

Expected: every unit test from Plans 1–2 PASSES.

- [x] **Step 9: Commit**

```bash
git add include/halcyon/halcyon.hpp docker/docker-compose.yml \
        tests/integration/CMakeLists.txt tests/integration/test_db2_roundtrip.cpp \
        CMakeLists.txt tests/CMakeLists.txt
git commit -m "test: add umbrella header and Dockerized Db2 integration suite"
```

---

## Self-Review

**Spec coverage (Plan 2 scope):**
- Real `Db2CliDriver` over `sqlcli1.h`, links `DB2::CLI`, RPATH baked → Task 8. ✔
- `Connection`/`Statement`/`ResultSet`/`Row` → Tasks 4–5. ✔
- `TypeBinder<T>` mapping table (integers, floating, bool, string, bytes, optional,
  bind-only string_view/char*) → Task 2. ✔
- Named + anonymous parameter binding → Task 3 (+ Connection overloads in Task 5). ✔
- `row.as<...>()` tuple mapping + range iteration → Task 4. ✔
- `HALCYON_REFLECT` + `queryAs<T>` → Task 6. ✔
- SQLSTATE → `ErrorCode` classification (`08xxx`/`-30081`/`40001`/`57033`/`23xxx`/
  `42xxx`/other), unit-tested without a DB → Task 7. ✔
- Dockerized Db2 integration tests, CTest label `integration`, gated → Task 9. ✔
- Mockability invariant preserved: the entire core is tested via `MockCliDriver`;
  only `db2_cli_driver.cpp` includes IBM CLI headers. ✔

**Type consistency:** `detail::cli::Value`/`Null`/`StatementHandle` defined in
Task 1 are used unchanged in Tasks 2–8. `TypeBinder<T>::to_value`/`from_value`
(Task 2) feed `pack_params`/`bind_named` (Task 3), `Row::try_as`/`as` (Task 4),
and `reflect::map_row` (Task 6). `ICliDriver`'s new methods (Task 1) are
implemented by both `MockCliDriver` (Task 1) and `Db2CliDriver` (Task 8) with
identical signatures. `classify_sqlstate` (Task 7) is the single classifier used
by `make_error` (Task 8) and matches the spec table. `ErrorCode::Mapping` →
`QueryException` (Plan 1's `throw_error`) makes `Row::as` throw `MappingException`
as asserted in Task 4.

**Deferred (intentionally, plug into the same patterns later):**
- `std::chrono` date/time and `halcyon::Decimal` `TypeBinder`s (the boundary
  carries them as strings today; add specializations + boundary alternatives).
- Named-column access (`row["name"]`) — `columnName` is already exposed for it.
- Per-connection prepared-statement caching (spec non-goal for v1).
- Pool, async executor, transactions, facade, bulk, observability → Plans 3–5.

**Placeholder scan:** No TBD/TODO/"handle errors here" — every code step is
complete. Two steps (Task 4 Step 4, Task 5 Step 4) intentionally *replace* an
earlier listing with the final, robust version and say so explicitly.

---

## Roadmap: subsequent plans (unchanged from Plan 1)

- **Plan 3 — Pool & concurrency:** `ConnectionPool` (`PoolConfig`, validation,
  bounded growth, background reaper), transparent reconnect with backoff, safe
  auto-retry policy (`ExecPolicy`, idempotent classification), `std::future`
  executor with a single `submit` chokepoint for future coroutine support.
- **Plan 4 — Facade & ergonomics:** `Database::open`/`openOrThrow`, OO `query`/
  `execute`/`queryAs`, functional free functions, RAII `Transaction` + functional
  `transaction(...)`, `executeBatch`/`batchOf` bulk path, examples.
- **Plan 5 — Observability:** `MetricsSink`/`Tracer` no-op defaults, `prometheus-cpp`
  and `opentelemetry-cpp` adapters behind `HALCYON_WITH_PROMETHEUS`/`HALCYON_WITH_OTEL`,
  metric/span emission at query/transaction/pool boundaries.
