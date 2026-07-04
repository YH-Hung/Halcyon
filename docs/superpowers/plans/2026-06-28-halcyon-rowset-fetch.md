# Db2 CLI Rowset (Block) Fetch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the row-at-a-time read path with Db2 CLI rowset (block) fetch — the read-side mirror of array binding — unifying both read paths onto one `fetchBlock` seam method, with a transparent per-statement fallback for long/LOB columns.

**Architecture:** Add one seam method `fetchBlock(stmt, maxRows) -> Result<std::vector<std::vector<Value>>>` that returns a row-major block of neutral `Value`s (empty == end of cursor). Land it additively with a behavior-preserving per-row driver body first, rewire the portable layer (`ResultSet` block-buffers, `Row` views a materialized row, `collect`/`map_row` map from `Value`s), retire the old `fetch`/`getColumn` seam pair, then drop in true rowset binding below the seam. Public API is unchanged throughout.

**Tech Stack:** C++17, IBM Db2 CLI (`sqlcli1.h`), CMake, GoogleTest, Docker (Db2 integration), ThreadSanitizer.

**Spec:** `docs/superpowers/specs/2026-06-28-halcyon-rowset-fetch-design.md`

## Global Constraints

- **C++17 only.** No C++20 features. (AGENTS.md)
- **Thin CLI seam invariant:** only `src/detail/cli/` and `cmake/FindDB2CLI.cmake` may reference `sqlcli1.h`/`SQL*` types/the `db2` library. Nothing above `ICliDriver` may leak CLI types. (AGENTS.md)
- **Dual error model:** recoverable paths return `Result<T>`; never let CLI codes escape the seam un-translated (map SQLSTATE → `ErrorCode` in `detail::cli`). (AGENTS.md)
- **Style:** `-Wall -Wextra -Wpedantic` clean; run `clang-format` + `clang-tidy` before committing; `#pragma once`; `lower_snake_case` files, `PascalCase` types, `camelCase` public methods. (AGENTS.md)
- **TDD:** failing test → see it fail → minimal impl → see it pass → commit. Small, focused commits with conventional prefixes (`feat:`/`fix:`/`test:`/`docs:`). (AGENTS.md)
- **Commit only when a step says to.** Do not push. (AGENTS.md)
- **Verification before "done" runs ALL tests including the live integration suite** (`HALCYON_BUILD_INTEGRATION_TESTS=ON`, container up, `HALCYON_TEST_DSN` set, `ctest -L integration`). A `Skipped` integration result is NOT verification. (AGENTS.md)
- **Fixed internal tunables (no public API):** `kFetchBlockBytes` ≈ 2 MiB, `kMaxBlockRows` = 4096, `kMaxBoundColBytes` = 64 KiB, portable request `kFetchBlockRows` = 4096. (spec §1, §3)
- Existing build dirs in use this session: `build` (unit), `build-it` (integration), `build-tsan` (stress). (AGENTS.md, memories)

---

## Task 1: Add `fetchBlock` to the seam (additive, behavior-preserving)

Introduce the new seam method alongside the existing `fetch`/`getColumn`. The driver implements it as a per-row loop over its own read helpers, so observable behavior is unchanged. Mock and stress double implement it. Unit-level only.

**Files:**
- Modify: `include/halcyon/detail/cli/driver.hpp`
- Modify: `src/detail/cli/db2_cli_driver.cpp`
- Modify: `tests/unit/mock_cli_driver.hpp`
- Modify: `tests/stress/support/concurrent_fake_driver.hpp`
- Test: `tests/unit/test_cli_seam.cpp`

**Interfaces:**
- Produces: `virtual Result<std::vector<std::vector<Value>>> ICliDriver::fetchBlock(StatementHandle, std::size_t maxRows)` — returns 1..maxRows rows on success, empty vector at clean end, classified `Error` on driver failure.
- Consumes: existing `columnCount`, `fetch`, `getColumn` (still present this task).

- [ ] **Step 1: Write the failing seam unit tests**

Append to `tests/unit/test_cli_seam.cpp`:

```cpp
TEST(CliSeamBlock, FetchBlockReturnsAllRowsThenEmpty) {
    MockCliDriver driver;
    auto conn = driver.connect(ConnectionParams{"x"}).value();
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id", "name"},
        {{Value{std::int64_t{1}}, Value{std::string{"ada"}}},
         {Value{std::int64_t{2}}, Value{Null{}}}}});
    auto st = driver.prepare(conn, "SELECT id, name FROM t").value();
    ASSERT_TRUE(driver.execute(st).ok());

    auto blk = driver.fetchBlock(st, 100);
    ASSERT_TRUE(blk.ok());
    ASSERT_EQ(blk.value().size(), 2u);
    EXPECT_EQ(blk.value()[0][0], Value{std::int64_t{1}});
    EXPECT_EQ(blk.value()[1][1], Value{Null{}});

    auto end = driver.fetchBlock(st, 100);
    ASSERT_TRUE(end.ok());
    EXPECT_TRUE(end.value().empty());  // exhausted -> empty
}

TEST(CliSeamBlock, FetchBlockHonorsMaxRowsAndResumes) {
    MockCliDriver driver;
    auto conn = driver.connect(ConnectionParams{"x"}).value();
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"},
        {{Value{std::int64_t{1}}}, {Value{std::int64_t{2}}},
         {Value{std::int64_t{3}}}}});
    auto st = driver.prepare(conn, "SELECT id FROM t").value();
    ASSERT_TRUE(driver.execute(st).ok());

    auto a = driver.fetchBlock(st, 2);
    ASSERT_TRUE(a.ok());
    ASSERT_EQ(a.value().size(), 2u);  // capped at maxRows
    auto b = driver.fetchBlock(st, 2);
    ASSERT_TRUE(b.ok());
    ASSERT_EQ(b.value().size(), 1u);  // remainder
    EXPECT_EQ(b.value()[0][0], Value{std::int64_t{3}});
}

TEST(CliSeamBlock, FetchBlockPropagatesScriptedError) {
    MockCliDriver driver;
    auto conn = driver.connect(ConnectionParams{"x"}).value();
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"}, {{Value{std::int64_t{1}}}}});
    Error e; e.code = ErrorCode::Connection; e.message = "drop";
    driver.fetchBlockError = e;
    driver.failFetchBlockOnCall = 1;
    auto st = driver.prepare(conn, "SELECT id FROM t").value();
    ASSERT_TRUE(driver.execute(st).ok());

    auto blk = driver.fetchBlock(st, 100);
    ASSERT_FALSE(blk.ok());
    EXPECT_EQ(blk.error().code, ErrorCode::Connection);
}
```

- [ ] **Step 2: Build to confirm it fails to compile**

Run: `cmake --build build -j 2>&1 | head -20`
Expected: errors — `ICliDriver`/`MockCliDriver` have no `fetchBlock`, no `fetchBlockError`/`failFetchBlockOnCall`.

- [ ] **Step 3: Declare the seam method**

In `include/halcyon/detail/cli/driver.hpp`, immediately after the `executeBatch` declaration (driver.hpp:60–62), add:

```cpp
    // Fetches up to maxRows rows from the open cursor as a row-major block of
    // neutral Values. Returns 1..maxRows rows on success; a short block does NOT
    // signal end. Returns an empty block exactly when the cursor is exhausted.
    // Returns a classified Error on a driver failure (e.g. mid-stream drop).
    virtual Result<std::vector<std::vector<Value>>> fetchBlock(
        StatementHandle stmt, std::size_t maxRows) = 0;
```

- [ ] **Step 4: Add private read helpers + provisional `fetchBlock` in the Db2 driver**

In `src/detail/cli/db2_cli_driver.cpp`: keep the existing public `fetch` (db2_cli_driver.cpp:438) and `getColumn` (db2_cli_driver.cpp:449) overrides, but extract their bodies into private helpers and have the overrides delegate. Replace the existing `fetch` and `getColumn` methods with:

```cpp
    Result<bool> fetch(StatementHandle stmt) override {
        StmtState* st = stmt_state(stmt);
        if (!st) return unknown_stmt();
        return fetch_row(st->handle);
    }

    Result<Value> getColumn(StatementHandle stmt, std::size_t index) override {
        StmtState* st = stmt_state(stmt);
        if (!st) return unknown_stmt();
        return read_column(st->handle, index);
    }

    Result<std::vector<std::vector<Value>>> fetchBlock(
        StatementHandle stmt, std::size_t maxRows) override {
        StmtState* st = stmt_state(stmt);
        if (!st) return unknown_stmt();
        auto cc = columnCount(stmt);
        if (!cc.ok()) return cc.error();
        const std::size_t ncols = cc.value();
        std::vector<std::vector<Value>> out;
        while (out.size() < maxRows) {
            auto f = fetch_row(st->handle);
            if (!f.ok()) return f.error();
            if (!f.value()) break;  // clean end of cursor
            std::vector<Value> row;
            row.reserve(ncols);
            for (std::size_t c = 0; c < ncols; ++c) {
                auto v = read_column(st->handle, c);
                if (!v.ok()) return v.error();
                row.push_back(std::move(v.value()));
            }
            out.push_back(std::move(row));
        }
        return out;
    }
```

Then add the two private helpers in the `private:` section (e.g. after `unknown_stmt`, db2_cli_driver.cpp:560). `fetch_row` is the former `fetch` body; `read_column` is the former `getColumn` body (the full type switch from db2_cli_driver.cpp:454–498):

```cpp
    // One-row cursor advance (was the body of fetch()).
    Result<bool> fetch_row(SQLHSTMT h) {
        SQLRETURN rc = SQLFetch(h);
        if (rc == SQL_NO_DATA) return false;
        if (!cli_ok(rc))
            return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                              "SQLFetch failed");
        return true;
    }

    // Reads the current row's 0-based column as a neutral Value (was the body of
    // getColumn()). Describes the column per call for now; Task 4 caches it.
    Result<Value> read_column(SQLHSTMT h, std::size_t index) {
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
                SQLBIGINT v = 0; SQLLEN ind = 0;
                if (!cli_ok(SQLGetData(h, col, SQL_C_SBIGINT, &v, sizeof(v), &ind)))
                    return make_error(SQL_HANDLE_STMT, h, ErrorCode::Unknown,
                                      "SQLGetData int failed");
                if (ind == SQL_NULL_DATA) return Value{Null{}};
                return Value{static_cast<std::int64_t>(v)};
            }
            case SQL_REAL:
            case SQL_FLOAT:
            case SQL_DOUBLE: {
                double v = 0; SQLLEN ind = 0;
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
```

- [ ] **Step 5: Implement `fetchBlock` in `MockCliDriver`**

In `tests/unit/mock_cli_driver.hpp`, after the `getColumn` block (mock_cli_driver.hpp:203), add the scripting fields + method:

```cpp
    // --- block fetch scripting ---
    std::size_t fetchBlockSize = 0;   // 0 => up to maxRows; >0 => cap per call
    Error fetchBlockError;
    int failFetchBlockOnCall = 0;     // 1-based fetchBlock() call to fail; 0=never
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
```

Add `#include <algorithm>` to the includes block (mock_cli_driver.hpp:3) for `std::min`.

- [ ] **Step 6: Implement `fetchBlock` in `ConcurrentFakeDriver`**

In `tests/stress/support/concurrent_fake_driver.hpp`, after `getColumn` (concurrent_fake_driver.hpp:162–168), add (it yields exactly one encoded row per cursor, mirroring `fetch`):

```cpp
    Result<std::vector<std::vector<Value>>> fetchBlock(
        StatementHandle stmt, std::size_t maxRows) override {
        (void)maxRows;
        const std::lock_guard<std::mutex> g(mu_);
        auto it = stmts_.find(stmt);
        if (it == stmts_.end()) return mapping<std::vector<std::vector<Value>>>();
        std::vector<std::vector<Value>> out;
        if (it->second.position < 0) {        // single row, once
            it->second.position = 0;
            out.push_back({Value{encoded_value(it->second.sql)}});
        }
        return Result<std::vector<std::vector<Value>>>(std::move(out));
    }
```

(If the fake's lock/member names differ, match the surrounding methods — `fetch` at concurrent_fake_driver.hpp:153 shows the exact `mu_`/`stmts_`/`mapping<T>()` usage.)

- [ ] **Step 7: Build and run the new seam tests**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R CliSeamBlock`
Expected: all three `CliSeamBlock.*` PASS.

- [ ] **Step 8: Run the full unit suite (no regressions; old path untouched)**

Run: `ctest --test-dir build --output-on-failure`
Expected: all PASS.

- [ ] **Step 9: Commit**

```bash
git add include/halcyon/detail/cli/driver.hpp src/detail/cli/db2_cli_driver.cpp \
        tests/unit/mock_cli_driver.hpp tests/stress/support/concurrent_fake_driver.hpp \
        tests/unit/test_cli_seam.cpp
git commit -m "feat: add fetchBlock seam method (behavior-preserving per-row body)"
```

---

## Task 2: Rewire the portable read path onto `fetchBlock`

`ResultSet` block-buffers; `Row` views a materialized row; `collect`/`map_row` map from `Value`s. Behavior is identical from the consumer's view, verified by the existing (rewritten) read unit tests. Still unit-level.

**Files:**
- Modify: `include/halcyon/connection.hpp`
- Modify: `include/halcyon/types.hpp` (`reflect::assign_fields`, `reflect::map_row`)
- Test: `tests/unit/test_result_set.cpp`, `tests/unit/test_reflect.cpp`

**Interfaces:**
- Consumes: `ICliDriver::fetchBlock` (Task 1).
- Produces: `Row(const std::vector<detail::cli::Value>& cells)`; `reflect::map_row<T>(const std::vector<detail::cli::Value>& cells) -> Result<T>`; `ResultSet` with block buffering. `detail::kFetchBlockRows` (portable request size).

- [ ] **Step 1: Rewrite the read unit tests (the failing spec)**

Replace the `MidStreamFetchErrorIsSurfacedNotSilentlyDropped` test in `tests/unit/test_result_set.cpp` (lines 61–86) with a block-fetch version, and add a multi-block test. New/changed cases:

```cpp
TEST(ResultSetTest, MidStreamFetchErrorIsSurfacedNotSilentlyDropped) {
    MockCliDriver driver;
    auto conn = driver.connect({"x"}).value();
    driver.resultSets.push_back(usersGrid());  // 2 rows available
    driver.fetchBlockSize = 1;                  // one row per block
    halcyon::Error e;
    e.code = halcyon::ErrorCode::Connection;
    e.message = "boom";
    e.retriable = true;
    driver.fetchBlockError = e;
    driver.failFetchBlockOnCall = 2;  // block 1 (row 0) ok; block 2 errors
    auto stmt = driver.prepare(conn, "SELECT id, name FROM u").value();
    ASSERT_TRUE(driver.execute(stmt).ok());

    auto rs = ResultSet::create_borrowing(driver, stmt).value();
    int rows = 0;
    for (auto& row : rs) { (void)row; ++rows; }
    EXPECT_EQ(rows, 1);
    ASSERT_FALSE(rs.ok());
    ASSERT_TRUE(rs.error().has_value());
    EXPECT_EQ(rs.error()->code, halcyon::ErrorCode::Connection);
    EXPECT_EQ(rs.error()->message, "boom");
}

TEST(ResultSetTest, RowsSpanMultipleBlocks) {
    MockCliDriver driver;
    auto conn = driver.connect({"x"}).value();
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"},
        {{Value{std::int64_t{1}}}, {Value{std::int64_t{2}}},
         {Value{std::int64_t{3}}}, {Value{std::int64_t{4}}}}});
    driver.fetchBlockSize = 2;  // forces >= 2 blocks for 4 rows
    auto stmt = driver.prepare(conn, "SELECT id FROM u").value();
    ASSERT_TRUE(driver.execute(stmt).ok());
    auto rs = ResultSet::create_borrowing(driver, stmt).value();

    std::vector<int> got;
    for (auto& row : rs) got.push_back(std::get<0>(row.as<int>()));
    EXPECT_EQ(got, (std::vector<int>{1, 2, 3, 4}));
    EXPECT_TRUE(rs.ok());
}
```

The other tests in the file (`RangeIterationYieldsTuples`, `TryAsReturnsErrorOnArityMismatch`, `CleanIterationLeavesNoError`, `AsThrowsMappingExceptionOnNullIntoNonOptional`) compile and pass unchanged — they exercise the same public surface. Leave them as-is.

- [ ] **Step 2: Build to confirm failure**

Run: `cmake --build build -j 2>&1 | head -20`
Expected: pass for now if the mock has the fields (Task 1), but the tests assert block semantics the *old* `ResultSet` (using `fetch`/`getColumn`) does not yet honor — `MidStreamFetchErrorIsSurfacedNotSilentlyDropped` references `fetchBlockSize`/`fetchBlockError` and expects the error via `fetchBlock`, which the old `ResultSet` never calls. Run the tests:
Run: `ctest --test-dir build --output-on-failure -R 'ResultSetTest.MidStream|ResultSetTest.RowsSpan'`
Expected: FAIL — old `ResultSet` still drives `fetch`/`getColumn`, so the scripted `fetchBlock` error never fires.

- [ ] **Step 3: Re-point `reflect::map_row` to map from `Value`s**

In `include/halcyon/types.hpp`, replace `assign_fields` (types.hpp:411–440) and `map_row` (types.hpp:443–456) with versions that take an in-hand row:

```cpp
// Assigns each column (by position) into the struct via its member pointers.
template <class T, class Tuple, std::size_t... I>
Result<void> assign_fields(T& out, const Tuple& mptrs,
                           const std::vector<detail::cli::Value>& cells,
                           std::index_sequence<I...>) {
    Error err;
    bool ok = true;
    auto step = [&](auto idx) {
        if (!ok) return;
        constexpr std::size_t i = decltype(idx)::value;
        auto mp = std::get<i>(mptrs);
        using F = std::remove_reference_t<decltype(out.*mp)>;
        auto v = TypeBinder<F>::from_value(cells[i]);
        if (!v.ok()) { ok = false; err = v.error(); return; }
        out.*mp = std::move(v.value());
    };
    (step(std::integral_constant<std::size_t, I>{}), ...);
    if (!ok) return err;
    return Result<void>();
}

// Maps an already-materialized row into a freshly value-initialized T.
template <class T>
Result<T> map_row(const std::vector<detail::cli::Value>& cells) {
    static_assert(Reflected<T>::value,
                  "queryAs<T> requires HALCYON_REFLECT(T, fields...)");
    constexpr std::size_t N = Reflected<T>::field_count;
    if (cells.size() != N)
        return detail::mapping_error("queryAs<T>: column count != field count");
    auto mptrs = Reflected<T>::members();
    T out{};
    auto r = assign_fields(out, mptrs, cells, std::make_index_sequence<N>{});
    if (!r.ok()) return r.error();
    return out;
}
```

- [ ] **Step 4: Rewrite `Row` as a view over a materialized row**

In `include/halcyon/connection.hpp`, replace the entire `Row` class (connection.hpp:29–100) with:

```cpp
// A non-owning view of one already-materialized result row (a vector of neutral
// Values produced by the driver's block fetch). Valid while its owning block is.
class Row {
public:
    explicit Row(const std::vector<detail::cli::Value>& cells) : cells_(&cells) {}

    std::size_t column_count() const noexcept { return cells_->size(); }

    template <class... Ts>
    Result<std::tuple<Ts...>> try_as() const {
        static_assert((is_readable<Ts>::value && ...),
                      "every column type must have a readable TypeBinder");
        if (sizeof...(Ts) != cells_->size())
            return detail::mapping_error("row.as<>: column count mismatch");
        return read_tuple<Ts...>(std::index_sequence_for<Ts...>{});
    }

    template <class... Ts>
    std::tuple<Ts...> as() const { return try_as<Ts...>().value(); }

private:
    template <class... Ts, std::size_t... I>
    Result<std::tuple<Ts...>> read_tuple(std::index_sequence<I...>) const {
        std::tuple<Ts...> out;
        Error err;
        bool ok = true;
        auto step = [&](auto idx, auto* slot) {
            if (!ok) return;
            constexpr std::size_t i = decltype(idx)::value;
            using F = std::remove_pointer_t<decltype(slot)>;
            auto v = TypeBinder<F>::from_value((*cells_)[i]);
            if (!v.ok()) { ok = false; err = v.error(); return; }
            *slot = std::move(v.value());
        };
        (step(std::integral_constant<std::size_t, I>{}, &std::get<I>(out)), ...);
        if (!ok) return err;
        return out;
    }

    const std::vector<detail::cli::Value>* cells_;
};
```

This removes the `owner_`/`record_read_error` plumbing entirely. Also delete the now-orphaned out-of-line `Row::record_read_error` definition (connection.hpp:256–258).

- [ ] **Step 5: Add the portable block-size constant and re-point `ResultSet`**

In `include/halcyon/connection.hpp`, inside `namespace halcyon { namespace detail {` near the top (after the includes), add:

```cpp
namespace detail {
// Rows requested per fetchBlock call; the driver further clamps by its byte
// budget. Internal tunable, not public API.
inline constexpr std::size_t kFetchBlockRows = 4096;
}  // namespace detail
```

Replace the `ResultSet` class body (connection.hpp:158–252) so it block-buffers. Keep `create_borrowing`, `column_count`, `error`, `ok`, the iterator type, and the owning-`ResultSet` friendship; change the members and `advance()`:

```cpp
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

    const std::optional<Error>& error() const noexcept { return error_; }
    bool ok() const noexcept { return !error_.has_value(); }

    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = Row;
        using difference_type = std::ptrdiff_t;
        using pointer = const Row*;
        using reference = const Row&;

        iterator() = default;
        explicit iterator(ResultSet* rs) : rs_(rs), at_end_(false) { advance(); }

        const Row& operator*() const { return *row_; }
        const Row* operator->() const { return &*row_; }
        iterator& operator++() { advance(); return *this; }
        bool operator==(const iterator& o) const noexcept { return at_end_ == o.at_end_; }
        bool operator!=(const iterator& o) const noexcept { return !(*this == o); }

    private:
        void advance() {
            if (rs_->pos_ >= rs_->block_.size()) {
                if (rs_->exhausted_) { at_end_ = true; row_.reset(); return; }
                auto blk = rs_->driver_->fetchBlock(rs_->stmt_,
                                                    detail::kFetchBlockRows);
                if (!blk.ok()) {
                    rs_->error_ = blk.error();
                    if (rs_->lease_) rs_->lease_->poison();
                    rs_->exhausted_ = true;
                    at_end_ = true; row_.reset(); return;
                }
                if (blk.value().empty()) {
                    rs_->exhausted_ = true;
                    at_end_ = true; row_.reset(); return;
                }
                rs_->block_ = std::move(blk.value());
                rs_->pos_ = 0;
            }
            row_.emplace(rs_->block_[rs_->pos_++]);
        }
        ResultSet* rs_ = nullptr;
        bool at_end_ = true;
        std::optional<Row> row_;
    };

    iterator begin() { return iterator(this); }
    iterator end() { return iterator(); }

private:
    ResultSet(detail::cli::ICliDriver* driver, detail::cli::StatementHandle stmt,
              std::size_t columns)
        : driver_(driver), stmt_(stmt), columns_(columns) {}

    friend class Connection;  // owning-ResultSet construction (run_query)
    detail::cli::ICliDriver* driver_;
    detail::cli::StatementHandle stmt_;
    std::size_t columns_;
    std::vector<std::vector<detail::cli::Value>> block_;
    std::size_t pos_ = 0;
    bool exhausted_ = false;
    std::optional<Error> error_;
    std::optional<detail::StatementLease> lease_;
};
```

Delete the obsolete out-of-line `Row::record_read_error` (Step 4) and the `note_column_error` method, which no longer exist.

- [ ] **Step 6: Re-point `Connection::collect<T>` to drain blocks**

In `include/halcyon/connection.hpp`, replace the body of `collect<T>` (connection.hpp:409–444) with:

```cpp
    template <class T>
    Result<std::vector<T>> collect(
        detail::StatementLease& lease,
        const std::vector<detail::cli::Value>& params) {
        auto b = driver_->bindParams(lease.handle(), params);
        if (!b.ok()) { lease.poison(); return b.error(); }
        auto e = driver_->execute(lease.handle());
        if (!e.ok()) { lease.poison(); return e.error(); }
        std::vector<T> out;
        for (;;) {
            auto blk = driver_->fetchBlock(lease.handle(), detail::kFetchBlockRows);
            if (!blk.ok()) { lease.poison(); return blk.error(); }
            if (blk.value().empty()) break;  // clean end
            for (auto& cells : blk.value()) {
                auto row = reflect::map_row<T>(cells);
                if (!row.ok()) return row.error();  // client-side mapping error
                out.push_back(std::move(row.value()));
            }
        }
        return out;
    }
```

(The `columnCount` call is dropped — `map_row` now checks arity against `cells.size()`.)

- [ ] **Step 7: Build and run the read tests**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R 'ResultSetTest|Reflect|Database'`
Expected: all PASS, including the new `MidStream`/`RowsSpanMultipleBlocks`.

- [ ] **Step 8: Run the full unit suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all PASS.

- [ ] **Step 9: Commit**

```bash
git add include/halcyon/connection.hpp include/halcyon/types.hpp \
        tests/unit/test_result_set.cpp tests/unit/test_reflect.cpp
git commit -m "feat: route the portable read path through fetchBlock (block-buffered ResultSet)"
```

---

## Task 3: Retire the `fetch`/`getColumn` seam pair

Nothing above the seam calls `fetch`/`getColumn` anymore. Remove them from `ICliDriver` and the doubles; keep the Db2 driver's read logic as private helpers. Move `test_cli_seam`'s row-at-a-time assertions onto `fetchBlock`.

**Files:**
- Modify: `include/halcyon/detail/cli/driver.hpp`
- Modify: `src/detail/cli/db2_cli_driver.cpp`
- Modify: `tests/unit/mock_cli_driver.hpp`
- Modify: `tests/stress/support/concurrent_fake_driver.hpp`
- Test: `tests/unit/test_cli_seam.cpp`

**Interfaces:**
- Removes: `ICliDriver::fetch`, `ICliDriver::getColumn`.
- Keeps: `Db2CliDriver::fetch_row`, `Db2CliDriver::read_column` (now purely private, used by `fetchBlock`'s fallback).

- [ ] **Step 1: Convert `test_cli_seam`'s fetch/getColumn test to fetchBlock**

In `tests/unit/test_cli_seam.cpp`, replace `CliSeamStatement.ScriptedResultSetIsFetchable` (lines 77–96) with:

```cpp
TEST(CliSeamStatement, ScriptedResultSetIsBlockFetchable) {
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

    auto blk = driver.fetchBlock(st, 100);
    ASSERT_TRUE(blk.ok());
    ASSERT_EQ(blk.value().size(), 2u);
    EXPECT_EQ(blk.value()[0][0], Value{std::int64_t{1}});
    EXPECT_EQ(blk.value()[0][1], Value{std::string{"ada"}});
    EXPECT_EQ(blk.value()[1][1], Value{Null{}});
    EXPECT_TRUE(driver.fetchBlock(st, 100).value().empty());  // end
}
```

- [ ] **Step 2: Remove the seam declarations**

In `include/halcyon/detail/cli/driver.hpp`, delete the `fetch` (driver.hpp:76–77) and `getColumn` (driver.hpp:79–80) pure-virtual declarations and their comments. Leave `columnCount`, `columnName`, `closeCursor`, `finalize`.

- [ ] **Step 3: Demote the Db2 driver methods to private helpers**

In `src/detail/cli/db2_cli_driver.cpp`, delete the public `fetch`/`getColumn` `override` methods added in Task 1 Step 4 (they only delegated). Keep `fetch_row` and `read_column` as the private helpers (already added in Task 1). `fetchBlock` already calls them — no further change.

- [ ] **Step 4: Remove fetch/getColumn from the doubles**

In `tests/unit/mock_cli_driver.hpp`, delete the `fetch` method + its injection fields (mock_cli_driver.hpp:171–184) and the `getColumn` method + its injection fields (mock_cli_driver.hpp:186–203). The `StmtState::position` field stays (used by `fetchBlock`).

In `tests/stress/support/concurrent_fake_driver.hpp`, delete the `fetch` (concurrent_fake_driver.hpp:153–160) and `getColumn` (concurrent_fake_driver.hpp:162–168) overrides. `fetchBlock` (Task 1) remains.

- [ ] **Step 5: Build to catch any lingering references**

Run: `cmake --build build -j 2>&1 | head -30`
Expected: clean build. If any test or source still names `.fetch(`/`.getColumn(` on a driver, it surfaces here — fix by switching to `fetchBlock` (grep: `grep -rn 'driver.*\.\(fetch\|getColumn\)(' tests include src`).

- [ ] **Step 6: Run the full unit suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all PASS.

- [ ] **Step 7: Build the stress suite to keep the double honest**

Run: `cmake --build build --target halcyon_stress_tests -j 2>&1 | tail -5` (or the stress target name from `tests/stress/CMakeLists.txt` if different).
Expected: compiles (the fake implements the new seam, not the removed methods).

- [ ] **Step 8: Commit**

```bash
git add include/halcyon/detail/cli/driver.hpp src/detail/cli/db2_cli_driver.cpp \
        tests/unit/mock_cli_driver.hpp tests/stress/support/concurrent_fake_driver.hpp \
        tests/unit/test_cli_seam.cpp
git commit -m "refactor: retire fetch/getColumn seam pair in favor of fetchBlock"
```

---

## Task 4: True Db2 CLI rowset binding in the driver

Replace the provisional per-row `fetchBlock` body with describe-once + rowset binding for all-bounded result sets, and a per-statement fallback (the existing per-row helpers) when any column is long/LOB. Verified by live integration tests.

**Files:**
- Modify: `src/detail/cli/db2_cli_driver.cpp`
- Test: `tests/integration/test_db2_roundtrip.cpp`

**Interfaces:**
- Consumes: `StmtState`, `fetch_row`, `read_column`, `make_error`, `cli_ok`, `get_string`, `get_binary`.
- Produces: identical `fetchBlock` contract; internal `StmtState::cols`/`blockFallback` cache, `describe_columns`, `reset_rowset`.

- [ ] **Step 1: Add the failing integration tests**

Append to `tests/integration/test_db2_roundtrip.cpp` (free `TEST`s; reuse the file's existing `dsn()` helper and a `BatchCount`-style count struct — define one here if not visible):

```cpp
// --- Rowset (block) fetch (Task 4) ---
namespace {
struct RowsetRow {
    std::int64_t id;
    std::string name;
    std::optional<std::int64_t> qty;
};
struct CountRow { std::int64_t c; };
}  // namespace
HALCYON_REFLECT(RowsetRow, id, name, qty);
HALCYON_REFLECT(CountRow, c);

TEST(Db2RowsetFetch, MultiBlockSelectPreservesOrderValuesAndNulls) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";
    auto dbh = halcyon::Database::open(*d);
    ASSERT_TRUE(dbh.ok()) << dbh.error().message;
    auto& h = dbh.value();
    h.execute("DROP TABLE halcyon_rowset");
    ASSERT_TRUE(h.execute(
                     "CREATE TABLE halcyon_rowset(id BIGINT NOT NULL, "
                     "name VARCHAR(32), qty BIGINT)").ok());

    const std::int64_t kRows = 10000;  // > one block: forces multiple fetches
    std::vector<std::tuple<std::int64_t, std::string>> seed;
    seed.reserve(kRows);
    for (std::int64_t i = 0; i < kRows; ++i)
        seed.emplace_back(i, "name-" + std::to_string(i));
    ASSERT_TRUE(h.executeBatch(
                     "INSERT INTO halcyon_rowset(id,name) VALUES (?,?)",
                     halcyon::batchOf(seed)).ok());

    auto rows = h.queryAsOrThrow<RowsetRow>(
        "SELECT id, name, qty FROM halcyon_rowset ORDER BY id");
    ASSERT_EQ(rows.size(), static_cast<std::size_t>(kRows));
    for (std::int64_t i = 0; i < kRows; ++i) {
        EXPECT_EQ(rows[static_cast<std::size_t>(i)].id, i);
        EXPECT_EQ(rows[static_cast<std::size_t>(i)].name,
                  "name-" + std::to_string(i));
        EXPECT_FALSE(rows[static_cast<std::size_t>(i)].qty.has_value());  // all NULL
    }
    h.execute("DROP TABLE halcyon_rowset");
}

TEST(Db2RowsetFetch, LongColumnFallbackMatchesBoundedProjection) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";
    auto dbh = halcyon::Database::open(*d);
    ASSERT_TRUE(dbh.ok()) << dbh.error().message;
    auto& h = dbh.value();
    h.execute("DROP TABLE halcyon_lob");
    ASSERT_TRUE(h.execute(
                     "CREATE TABLE halcyon_lob(id BIGINT NOT NULL, body CLOB)").ok());
    const std::string body(5000, 'z');
    ASSERT_TRUE(h.execute("INSERT INTO halcyon_lob(id, body) VALUES (?, ?)",
                          std::int64_t{1}, body).ok());

    // Bounded projection: rowset path. Same rows via the CLOB: fallback path.
    struct IdRow { std::int64_t id; };  // local reflect below
    auto bounded = h.queryAsOrThrow<CountRow>(
        "SELECT COUNT(*) AS c FROM halcyon_lob");
    EXPECT_EQ(bounded[0].c, 1);

    // The fallback path must return the full CLOB byte-identically.
    auto rs = h.query("SELECT id, body FROM halcyon_lob ORDER BY id");
    ASSERT_TRUE(rs.ok()) << rs.error().message;  // Result<ResultSet>::error() is Error&
    int seen = 0;
    for (auto& row : rs.value()) {
        auto [id, got] = row.as<std::int64_t, std::string>();
        EXPECT_EQ(id, 1);
        EXPECT_EQ(got, body);  // 5000 chars, intact through fallback
        ++seen;
    }
    EXPECT_EQ(seen, 1);
    ASSERT_TRUE(rs.value().ok());
    h.execute("DROP TABLE halcyon_lob");
}

TEST(Db2RowsetFetch, AllScalarTypesRoundTripThroughBlock) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";
    auto dbh = halcyon::Database::open(*d);
    ASSERT_TRUE(dbh.ok()) << dbh.error().message;
    auto& h = dbh.value();
    h.execute("DROP TABLE halcyon_types");
    ASSERT_TRUE(h.execute(
                     "CREATE TABLE halcyon_types(i BIGINT, d DOUBLE, s VARCHAR(16), "
                     "b VARCHAR(8) FOR BIT DATA, n BIGINT)").ok());
    std::vector<std::byte> bytes = {std::byte{0x00}, std::byte{0xFF},
                                    std::byte{0x10}};
    ASSERT_TRUE(h.execute(
                     "INSERT INTO halcyon_types(i,d,s,b,n) VALUES (?,?,?,?,?)",
                     std::int64_t{42}, 3.5, std::string{"hi"}, bytes,
                     std::optional<std::int64_t>{}).ok());

    auto rs = h.query("SELECT i, d, s, b, n FROM halcyon_types");
    ASSERT_TRUE(rs.ok());
    int seen = 0;
    for (auto& row : rs.value()) {
        auto [i, dd, s, b, n] = row.as<std::int64_t, double, std::string,
                                        std::vector<std::byte>,
                                        std::optional<std::int64_t>>();
        EXPECT_EQ(i, 42);
        EXPECT_DOUBLE_EQ(dd, 3.5);
        EXPECT_EQ(s, "hi");
        EXPECT_EQ(b, bytes);          // embedded 0x00 preserved
        EXPECT_FALSE(n.has_value());  // NULL via indicator
        ++seen;
    }
    EXPECT_EQ(seen, 1);
    h.execute("DROP TABLE halcyon_types");
}
```

- [ ] **Step 2: Bring up live Db2 and run the new tests against the provisional body**

```bash
export DOCKER_DEFAULT_PLATFORM=linux/amd64
docker compose -f docker/docker-compose.yml up -d
docker compose -f docker/docker-compose.yml ps   # wait for STATUS = healthy
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
cmake -S . -B build-it -DHALCYON_BUILD_TESTS=ON -DHALCYON_BUILD_INTEGRATION_TESTS=ON
cmake --build build-it -j
ctest --test-dir build-it --output-on-failure -L integration -R Db2RowsetFetch
```
Expected: all three PASS even on the provisional per-row body (it returns identical Values). This establishes the behavioral contract that the rowset rewrite must preserve. (If the GSKit libs are quarantined on macOS, clear quarantine first per AGENTS.md.)

- [ ] **Step 3: Add the column-metadata cache to `StmtState`**

In `src/detail/cli/db2_cli_driver.cpp`, extend `StmtState` (db2_cli_driver.cpp:546) and add a `ColMeta` struct just above it:

```cpp
    struct ColMeta {
        SQLSMALLINT sqlType = 0;
        SQLSMALLINT cType = SQL_C_CHAR;
        SQLLEN      bindWidth = 0;   // fixed size, or buffer width for var columns
        bool        bounded = true;
    };
    struct StmtState {
        SQLHSTMT handle;
        std::vector<BoundParam> bound;
        std::optional<std::vector<ColMeta>> cols;  // lazily described per cursor
        bool blockFallback = false;
    };
```

Add `#include <optional>` and `#include <cstring>` at the top if not present (`<cstring>` is already included for array binding). Reset the cache wherever a cursor is closed or re-executed: in `closeCursor` (db2_cli_driver.cpp:514) after the `SQLFreeStmt(SQL_CLOSE)`, and at the end of `execute` (db2_cli_driver.cpp ~line 226, the result-set producing path), set `st->cols.reset(); st->blockFallback = false;`. (Locate `execute`'s `StmtState` via `stmt_state`; add the reset right before it returns.)

- [ ] **Step 4: Add `describe_columns` + `reset_rowset` helpers**

In the `private:` section near `read_column`, add:

```cpp
    static constexpr std::size_t kMaxBoundColBytes = 64u * 1024;

    // Describes every result column once and caches type/width/bounded on the
    // statement. bounded=false (long/LOB or unknown/over-cap width) makes the
    // whole statement use the per-row fallback.
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
                case SQL_SMALLINT: case SQL_INTEGER: case SQL_BIGINT:
                    m.cType = SQL_C_SBIGINT; m.bindWidth = sizeof(SQLBIGINT);
                    break;
                case SQL_REAL: case SQL_FLOAT: case SQL_DOUBLE:
                    m.cType = SQL_C_DOUBLE; m.bindWidth = sizeof(double);
                    break;
                case SQL_BIT:
                    m.cType = SQL_C_BIT; m.bindWidth = 1;
                    break;
                case SQL_BINARY: case SQL_VARBINARY:
                    m.cType = SQL_C_BINARY;
                    m.bindWidth = static_cast<SQLLEN>(colSize);
                    break;
                case SQL_LONGVARBINARY: case SQL_BLOB: case SQL_CLOB:
                case SQL_LONGVARCHAR:
                    m.bounded = false;
                    break;
                default:  // CHAR/VARCHAR/DECIMAL/NUMERIC/DATE/TIME/TIMESTAMP -> text
                    m.cType = SQL_C_CHAR;
                    // +1 for the CLI's NUL terminator on SQL_C_CHAR reads.
                    m.bindWidth = static_cast<SQLLEN>(colSize) + 1;
                    break;
            }
            if (m.bounded && (m.bindWidth <= 0 ||
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
        SQLSetStmtAttr(h, SQL_ATTR_ROW_ARRAY_SIZE,  // NOLINT(performance-no-int-to-ptr)
            reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(1)), 0);
        SQLSetStmtAttr(h, SQL_ATTR_ROW_STATUS_PTR, nullptr, 0);
        SQLSetStmtAttr(h, SQL_ATTR_ROWS_FETCHED_PTR, nullptr, 0);
    }
```

If your `sqlcli1.h` lacks `SQL_CLOB`/`SQL_LONGVARCHAR` spellings, use the names it defines (Db2 CLI: `SQL_CLOB`, `SQL_BLOB` are in `sqlcli1.h`; verify via `grep -E 'SQL_CLOB|SQL_LONGVARCHAR|SQL_BLOB' third_party/clidriver/include/sqlcli1.h`).

- [ ] **Step 5: Replace `fetchBlock` with describe-once + rowset/fallback dispatch**

Replace the provisional `fetchBlock` (Task 1) body with:

```cpp
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
```

Add the fallback helper (the former per-row loop, now using cached metadata so it never re-describes):

```cpp
    Result<std::vector<std::vector<Value>>> fetch_block_fallback(
        StmtState* st, std::size_t maxRows) {
        const auto& cols = *st->cols;
        std::vector<std::vector<Value>> out;
        while (out.size() < maxRows) {
            auto f = fetch_row(st->handle);
            if (!f.ok()) return f.error();
            if (!f.value()) break;
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
```

Change `read_column(h, index)` (Task 1) to `read_column_typed(h, index, sqlType)` — drop its internal `SQLDescribeCol`, take `SQLSMALLINT sqlType` as a parameter, and keep the same type switch. (This is the describe-once win on the fallback path.)

- [ ] **Step 6: Implement the rowset path**

Add the bounded-column rowset helper:

```cpp
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
            buf[c].assign((nbytes + sizeof(std::int64_t) - 1) / sizeof(std::int64_t),
                          0);
            ind[c].assign(R, 0);
        }

        SQLULEN rowsFetched = 0;
        std::vector<SQLUSMALLINT> rowStatus(R, 0);
        SQLSetStmtAttr(h, SQL_ATTR_ROW_BIND_TYPE,  // NOLINT(performance-no-int-to-ptr)
            reinterpret_cast<SQLPOINTER>(
                static_cast<SQLULEN>(SQL_BIND_BY_COLUMN)), 0);
        SQLSetStmtAttr(h, SQL_ATTR_ROW_ARRAY_SIZE,  // NOLINT(performance-no-int-to-ptr)
            reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(R)), 0);
        SQLSetStmtAttr(h, SQL_ATTR_ROW_STATUS_PTR, rowStatus.data(), 0);
        SQLSetStmtAttr(h, SQL_ATTR_ROWS_FETCHED_PTR, &rowsFetched, 0);

        for (std::size_t c = 0; c < nCols; ++c) {
            SQLRETURN rc = SQLBindCol(
                h, static_cast<SQLUSMALLINT>(c + 1), cols[c].cType,
                buf[c].data(), cols[c].bindWidth, ind[c].data());
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
            return std::vector<std::vector<Value>>{};  // clean end
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
                if (li == SQL_NULL_DATA) { row.push_back(Value{Null{}}); continue; }
                switch (cols[c].cType) {
                    case SQL_C_SBIGINT: {
                        SQLBIGINT v; std::memcpy(&v, slot, sizeof(v));
                        row.push_back(Value{static_cast<std::int64_t>(v)});
                        break;
                    }
                    case SQL_C_DOUBLE: {
                        double v; std::memcpy(&v, slot, sizeof(v));
                        row.push_back(Value{v});
                        break;
                    }
                    case SQL_C_BIT:
                        row.push_back(Value{static_cast<std::int64_t>(
                            *slot ? 1 : 0)});
                        break;
                    case SQL_C_BINARY:
                        row.push_back(Value{std::vector<std::byte>(
                            reinterpret_cast<const std::byte*>(slot),
                            reinterpret_cast<const std::byte*>(slot) +
                                static_cast<std::size_t>(li))});
                        break;
                    default:  // SQL_C_CHAR text (string/decimal/date/time/ts)
                        row.push_back(Value{std::string(
                            slot, static_cast<std::size_t>(li))});
                        break;
                }
            }
            out.push_back(std::move(row));
        }
        reset_rowset(h);
        return out;
    }
```

Note the `SQL_C_BIT` read returns `int64{0|1}` to match the scalar `getColumn`/`read_column` contract (a boolean column is read back as `int64`, per the seam `Value` comment at driver.hpp:30). Verify `cType` for `SQL_C_BIT` reads a single byte at `slot`.

- [ ] **Step 7: Rebuild and run the rowset integration tests**

```bash
cmake --build build-it -j
ctest --test-dir build-it --output-on-failure -L integration -R Db2RowsetFetch
```
Expected: all three PASS — multi-block ordering/values/NULLs, CLOB fallback byte-identical, all scalar types (including embedded-NUL binary and NULL).

- [ ] **Step 8: Run the full integration suite for regressions**

Run: `ctest --test-dir build-it --output-on-failure -L integration`
Expected: every live test PASS, none skipped (existing roundtrip/array-binding tests still green — they read results through the same path).

- [ ] **Step 9: Run the full unit suite (mock path unaffected)**

Run: `ctest --test-dir build --output-on-failure`
Expected: all PASS.

- [ ] **Step 10: Commit**

```bash
git add src/detail/cli/db2_cli_driver.cpp tests/integration/test_db2_roundtrip.cpp
git commit -m "feat: true Db2 CLI rowset binding for fetchBlock with long-column fallback"
```

---

## Task 5: Read-throughput benchmark (live)

A standalone benchmark timing materialization of N rows, mirroring `batch_insert_bench.cpp`, to put numbers on spec §7/§8.

**Files:**
- Create: `tests/stress/perf/select_bench.cpp`
- Modify: `tests/stress/perf/CMakeLists.txt`

**Interfaces:**
- Consumes: `Database::openOrThrow`, `executeBatch`/`batchOf`, `queryAsOrThrow<T>`, `query`.

- [ ] **Step 1: Write the benchmark**

Create `tests/stress/perf/select_bench.cpp`:

```cpp
// Live read-throughput benchmark (spec §7/§8). Requires HALCYON_TEST_DSN.
// Seeds N rows of bounded columns, then times materializing them via the block
// fetch path two shapes: narrow (2 cols) and wide (8 cols). Prints rows/sec.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#include "halcyon/halcyon.hpp"

namespace {
struct Narrow { std::int64_t id; std::string name; };
double secs_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now() - t0).count();
}
}  // namespace
HALCYON_REFLECT(Narrow, id, name);

int main() {
    const char* dsn = std::getenv("HALCYON_TEST_DSN");
    if (!dsn) { std::cerr << "HALCYON_TEST_DSN not set; skipping\n"; return 0; }
    auto h = halcyon::Database::openOrThrow(std::string(dsn));
    const std::int64_t N = 200000;

    h.execute("DROP TABLE halcyon_read_bench");
    h.executeOrThrow(
        "CREATE TABLE halcyon_read_bench(id BIGINT NOT NULL, name VARCHAR(32))");
    std::vector<std::tuple<std::int64_t, std::string>> seed;
    seed.reserve(N);
    for (std::int64_t i = 0; i < N; ++i) seed.emplace_back(i, "name-" +
                                                           std::to_string(i));
    h.executeBatch("INSERT INTO halcyon_read_bench(id,name) VALUES (?,?)",
                   halcyon::batchOf(seed)).value();

    auto t0 = std::chrono::steady_clock::now();
    auto rows = h.queryAsOrThrow<Narrow>(
        "SELECT id, name FROM halcyon_read_bench");
    double mat = secs_since(t0);

    h.execute("DROP TABLE halcyon_read_bench");
    auto rps = [](std::int64_t n, double s) {
        return s > 0 ? static_cast<double>(n) / s : 0; };
    std::cout << "rows=" << rows.size() << "\n"
              << "block_fetch_materialize : " << mat << "s  "
              << rps(static_cast<std::int64_t>(rows.size()), mat)
              << " rows/s\n";
    return 0;
}
```

(Baseline for comparison: check out the commit before Task 2, build, and run the same binary, or note the pre-change numbers from a `git stash` build. Record both in §8.)

- [ ] **Step 2: Wire the build target**

In `tests/stress/perf/CMakeLists.txt`, mirror the `halcyon_batch_insert_bench` target lines:

```cmake
add_executable(halcyon_select_bench select_bench.cpp)
target_link_libraries(halcyon_select_bench PRIVATE halcyon::halcyon)
```

- [ ] **Step 3: Build the benchmark**

Run: `cmake --build build-it --target halcyon_select_bench -j`
Expected: links cleanly.

- [ ] **Step 4: Commit (running it happens in Final verification)**

```bash
git add tests/stress/perf/select_bench.cpp tests/stress/perf/CMakeLists.txt
git commit -m "test: live read-throughput benchmark for block fetch"
```

---

## Task 6: Documentation reconciliation

**Files:**
- Modify: `docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md` (§1 non-goals)
- Modify: `docs/guide/querying.md`
- Modify: `docs/guide/advanced.md`
- Modify: `README.md`

- [ ] **Step 1: Note the parent-spec non-goal is partly addressed**

In `docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md` §1 non-goals, change the "Large-result streaming / server-side cursors" bullet to:

```markdown
- Large-result *scrollable* cursors. (Forward block fetch now backs the read
  path — see docs/superpowers/specs/2026-06-28-halcyon-rowset-fetch-design.md —
  but scrollable/positionable cursors remain deferred.)
```

- [ ] **Step 2: Document block fetch in the querying guide**

In `docs/guide/querying.md`, add a short paragraph (no API change):

```markdown
## How rows arrive (block fetch)

Reads use Db2 CLI **rowset (block) fetch**: columns are bound once and the
driver fills a block of rows per round-trip, so `query`/`queryAs` collapse what
would be per-row, per-column CLI calls into a handful of fetches. This is
transparent — the API is unchanged. A result set that contains a large object
(`CLOB`/`BLOB`/`LONG VARCHAR`/very wide `VARCHAR`) transparently falls back to a
row-at-a-time read for correctness; everything else uses the fast path.
```

- [ ] **Step 3: Document the internal tunables in the advanced guide**

In `docs/guide/advanced.md`, add:

```markdown
## Fetch block tuning (internal)

Block fetch uses a fixed internal byte budget (~2 MiB) and row cap to size each
fetch, and a bounded-column width threshold (~64 KiB) above which a column is
treated as "long" and the statement falls back to row-at-a-time. These are
internal constants, not public API; they mirror the write path's array-binding
byte budget.
```

- [ ] **Step 4: Update the README feature list**

In `README.md`, under Features (README.md:15, next to the bulk/batch bullet), add:

```markdown
- **Block fetch** — reads use Db2 CLI rowset (block) binding (the read-side mirror of array binding); result sets with a LOB/long column transparently fall back to row-at-a-time
```

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md \
        docs/guide/querying.md docs/guide/advanced.md README.md
git commit -m "docs: describe read-path block fetch and its long-column fallback"
```

---

## Final verification

- [ ] **Full unit suite:** `ctest --test-dir build --output-on-failure` — all PASS.
- [ ] **Full live integration suite** (container up, `HALCYON_TEST_DSN` set):
  `ctest --test-dir build-it --output-on-failure -L integration` — all PASS, **none skipped**.
- [ ] **TSan stress clean:** rebuild the stress suite under ThreadSanitizer in `build-tsan` per `tests/stress/README.md` and run `ctest --test-dir build-tsan -L stress --output-on-failure` — no data-race reports (the read path now hands out block-backed `Row`s; confirm no races in the executor/cache).
- [ ] **Benchmark recorded:** run `./build-it/tests/stress/perf/halcyon_select_bench` (path per build layout) and a baseline from before Task 2; fill the results table (date, N, row shape, rows/sec, takeaway) into spec §8 "Results", then commit `docs: record block-fetch benchmark results`. If block fetch is not faster, investigate with superpowers:systematic-debugging before claiming done.
- [ ] **Examples + orders sample still build** against the changed headers: configure/build `examples` (`-DHALCYON_BUILD_EXAMPLES=ON`) and the `samples/orders` consumer; both compile and run green.
- [ ] **No stray references:** `grep -rn '\.\(fetch\|getColumn\)(' include src tests` returns only unrelated matches (e.g. `next_conn_.fetch_add`), no driver `fetch`/`getColumn` calls.
- [ ] **Tear down Db2:** `docker compose -f docker/docker-compose.yml down`.
- [ ] Use superpowers:requesting-code-review before merging the branch.
```
