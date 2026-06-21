# Halcyon Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Stand up the Halcyon build system, error/result core types, and the mockable CLI driver seam so the rest of the library can be built and unit-tested without a live Db2.

**Architecture:** Layered design (Approach A). This plan delivers the bottom of the stack: a CMake project that builds a `halcyon::halcyon` library, a vendored `Result<T>`/`Error` model with a dual functional/throwing API, and an `ICliDriver` interface plus a `MockCliDriver` for unit tests. No `sqlcli1.h` code is written yet (that is Plan 2); this plan defines the seam it will plug into.

**Tech Stack:** C++17, CMake ≥ 3.20, GoogleTest (via FetchContent for out-of-the-box test runs), IBM Db2 CLI driver vendored at `third_party/clidriver` (only referenced by the CMake find module here).

---

## File Structure (created by this plan)

- `CMakeLists.txt` — top-level project, options, library target, test wiring.
- `cmake/FindDB2CLI.cmake` — locates the vendored CLI driver (headers + `db2` lib).
- `include/halcyon/version.hpp` — version constants.
- `include/halcyon/error.hpp` — `ErrorCode`, `Error`, exception hierarchy, `throw_error`.
- `include/halcyon/result.hpp` — `Result<T>` and `Result<void>`.
- `include/halcyon/detail/cli/driver.hpp` — `ICliDriver` seam + handle/param types.
- `src/core/version.cpp` — version accessor (gives the lib a translation unit).
- `src/core/error.cpp` — `to_string(ErrorCode)`, `throw_error`.
- `tests/CMakeLists.txt` — GoogleTest setup + test registration.
- `tests/unit/test_version.cpp` — smoke test.
- `tests/unit/test_error.cpp` — error model tests.
- `tests/unit/test_result.cpp` — `Result<T>` tests.
- `tests/unit/mock_cli_driver.hpp` — `MockCliDriver` implementing `ICliDriver`.
- `tests/unit/test_cli_seam.cpp` — tests exercising the seam via the mock.

---

## Task 1: Project skeleton & test harness

**Files:**
- Create: `CMakeLists.txt`
- Create: `cmake/FindDB2CLI.cmake`
- Create: `include/halcyon/version.hpp`
- Create: `src/core/version.cpp`
- Create: `tests/CMakeLists.txt`
- Test: `tests/unit/test_version.cpp`

- [x] **Step 1: Write the version header**

Create `include/halcyon/version.hpp`:

```cpp
#pragma once

#include <string_view>

namespace halcyon {

inline constexpr int version_major = 0;
inline constexpr int version_minor = 1;
inline constexpr int version_patch = 0;

// Returns the semantic version string, e.g. "0.1.0".
std::string_view version() noexcept;

}  // namespace halcyon
```

- [x] **Step 2: Write the version source**

Create `src/core/version.cpp`:

```cpp
#include "halcyon/version.hpp"

namespace halcyon {

std::string_view version() noexcept {
    return "0.1.0";
}

}  // namespace halcyon
```

- [x] **Step 3: Write the DB2 CLI find module**

Create `cmake/FindDB2CLI.cmake`. Defaults to the vendored driver, overridable via
`-DDB2_CLIDRIVER_ROOT=...`. Produces an imported target `DB2::CLI`.

```cmake
# Locates the IBM Db2 CLI driver (headers + db2 shared library).
# Result: imported target DB2::CLI, plus DB2CLI_FOUND.

if(NOT DB2_CLIDRIVER_ROOT)
    set(DB2_CLIDRIVER_ROOT "${CMAKE_SOURCE_DIR}/third_party/clidriver"
        CACHE PATH "Root of the IBM Db2 CLI driver")
endif()

find_path(DB2CLI_INCLUDE_DIR
    NAMES sqlcli1.h
    HINTS "${DB2_CLIDRIVER_ROOT}/include"
    NO_DEFAULT_PATH)

find_library(DB2CLI_LIBRARY
    NAMES db2
    HINTS "${DB2_CLIDRIVER_ROOT}/lib"
    NO_DEFAULT_PATH)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DB2CLI
    REQUIRED_VARS DB2CLI_LIBRARY DB2CLI_INCLUDE_DIR)

if(DB2CLI_FOUND AND NOT TARGET DB2::CLI)
    add_library(DB2::CLI UNKNOWN IMPORTED)
    set_target_properties(DB2::CLI PROPERTIES
        IMPORTED_LOCATION "${DB2CLI_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DB2CLI_INCLUDE_DIR}")
    # Bake an RPATH so executables find libdb2 without LD_LIBRARY_PATH/DYLD_LIBRARY_PATH.
    set(DB2CLI_LIBRARY_DIR "${DB2_CLIDRIVER_ROOT}/lib" CACHE PATH "Db2 CLI lib dir")
endif()

mark_as_advanced(DB2CLI_INCLUDE_DIR DB2CLI_LIBRARY)
```

- [x] **Step 4: Write the top-level CMakeLists**

Create `CMakeLists.txt`. Note: `DB2::CLI` is located but not yet linked into the
library (no `sqlcli1.h` code until Plan 2); we resolve it now to fail fast if the
driver is missing and to expose the RPATH dir for later.

```cmake
cmake_minimum_required(VERSION 3.20)
project(Halcyon VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(HALCYON_WITH_PROMETHEUS "Build prometheus-cpp metrics adapter" OFF)
option(HALCYON_WITH_OTEL "Build opentelemetry-cpp tracing adapter" OFF)
option(HALCYON_BUILD_TESTS "Build tests" ON)
option(HALCYON_BUILD_EXAMPLES "Build examples" OFF)
option(HALCYON_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(DB2CLI REQUIRED)

add_library(halcyon
    src/core/version.cpp)
add_library(halcyon::halcyon ALIAS halcyon)

target_include_directories(halcyon
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(halcyon PUBLIC cxx_std_17)
target_compile_options(halcyon PRIVATE -Wall -Wextra -Wpedantic)
if(HALCYON_WARNINGS_AS_ERRORS)
    target_compile_options(halcyon PRIVATE -Werror)
endif()

if(HALCYON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

- [x] **Step 5: Write the tests CMakeLists with GoogleTest**

Create `tests/CMakeLists.txt`:

```cmake
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(halcyon_unit_tests
    unit/test_version.cpp)

target_link_libraries(halcyon_unit_tests
    PRIVATE halcyon::halcyon GTest::gtest_main)

target_include_directories(halcyon_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/unit)

include(GoogleTest)
gtest_discover_tests(halcyon_unit_tests)
```

- [x] **Step 6: Write the failing smoke test**

Create `tests/unit/test_version.cpp`:

```cpp
#include <gtest/gtest.h>

#include "halcyon/version.hpp"

TEST(Version, ReturnsSemanticVersionString) {
    EXPECT_EQ(halcyon::version(), "0.1.0");
}

TEST(Version, ConstantsMatchString) {
    EXPECT_EQ(halcyon::version_major, 0);
    EXPECT_EQ(halcyon::version_minor, 1);
    EXPECT_EQ(halcyon::version_patch, 0);
}
```

- [x] **Step 7: Configure and build**

Run:

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON
cmake --build build -j
```

Expected: configures (finds `DB2::CLI` from `third_party/clidriver`), compiles
`halcyon` and `halcyon_unit_tests` with no errors.

- [x] **Step 8: Run the tests to verify they pass**

Run: `ctest --test-dir build --output-on-failure`
Expected: `Version.ReturnsSemanticVersionString` and `Version.ConstantsMatchString` PASS.

- [x] **Step 9: Commit**

```bash
git add CMakeLists.txt cmake/FindDB2CLI.cmake include/halcyon/version.hpp \
        src/core/version.cpp tests/CMakeLists.txt tests/unit/test_version.cpp
git commit -m "build: project skeleton, DB2 CLI find module, and test harness"
```

---

## Task 2: Error model

**Files:**
- Create: `include/halcyon/error.hpp`
- Create: `src/core/error.cpp`
- Modify: `CMakeLists.txt` (add `src/core/error.cpp` to `add_library`)
- Modify: `tests/CMakeLists.txt` (add `unit/test_error.cpp`)
- Test: `tests/unit/test_error.cpp`

- [x] **Step 1: Write the failing test**

Create `tests/unit/test_error.cpp`:

```cpp
#include <gtest/gtest.h>

#include "halcyon/error.hpp"

using halcyon::Error;
using halcyon::ErrorCode;

TEST(ErrorModel, ToStringCoversAllCodes) {
    EXPECT_STREQ(halcyon::to_string(ErrorCode::Connection), "Connection");
    EXPECT_STREQ(halcyon::to_string(ErrorCode::Deadlock), "Deadlock");
    EXPECT_STREQ(halcyon::to_string(ErrorCode::Unknown), "Unknown");
}

TEST(ErrorModel, ThrowErrorMapsCodeToSubtype) {
    Error e;
    e.code = ErrorCode::Constraint;
    e.sqlstate = "23505";
    e.message = "duplicate key";
    EXPECT_THROW(halcyon::throw_error(e), halcyon::ConstraintException);
}

TEST(ErrorModel, ExceptionCarriesErrorAndMessage) {
    Error e;
    e.code = ErrorCode::Connection;
    e.sqlstate = "08001";
    e.message = "cannot connect";
    e.retriable = true;
    try {
        halcyon::throw_error(e);
        FAIL() << "expected throw";
    } catch (const halcyon::Exception& ex) {
        EXPECT_EQ(ex.error().code, ErrorCode::Connection);
        EXPECT_EQ(ex.error().sqlstate, "08001");
        EXPECT_TRUE(ex.error().retriable);
        EXPECT_STREQ(ex.what(), "cannot connect");
    }
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build -R ErrorModel --output-on-failure` (after adding the
file to `tests/CMakeLists.txt` and reconfiguring; build will fail to compile first).
Expected: compile FAIL — `halcyon/error.hpp` not found / symbols undefined.

- [x] **Step 3: Write the error header**

Create `include/halcyon/error.hpp`:

```cpp
#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace halcyon {

enum class ErrorCode {
    Ok,
    Connection,
    Timeout,
    Constraint,
    Syntax,
    Deadlock,
    Transient,
    Mapping,
    Pool,
    Unknown,
};

// Human-readable name for an ErrorCode (never null).
const char* to_string(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string sqlstate;       // raw 5-char SQLSTATE, e.g. "08001"
    int nativeError = 0;        // SQLCODE / native db2 code
    std::string message;        // diagnostic text
    bool retriable = false;     // transient class → drives auto-retry
};

class Exception : public std::runtime_error {
public:
    explicit Exception(Error err)
        : std::runtime_error(err.message), error_(std::move(err)) {}
    const Error& error() const noexcept { return error_; }

private:
    Error error_;
};

class ConnectionException : public Exception {
public:
    using Exception::Exception;
};
class QueryException : public Exception {
public:
    using Exception::Exception;
};
class ConstraintException : public Exception {
public:
    using Exception::Exception;
};
class TimeoutException : public Exception {
public:
    using Exception::Exception;
};
class TransientException : public Exception {
public:
    using Exception::Exception;
};
class MappingException : public Exception {
public:
    using Exception::Exception;
};

// Throws the exception subtype that corresponds to err.code. Never returns.
[[noreturn]] void throw_error(const Error& err);

}  // namespace halcyon
```

- [x] **Step 4: Write the error source**

Create `src/core/error.cpp`:

```cpp
#include "halcyon/error.hpp"

namespace halcyon {

const char* to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:         return "Ok";
        case ErrorCode::Connection: return "Connection";
        case ErrorCode::Timeout:    return "Timeout";
        case ErrorCode::Constraint: return "Constraint";
        case ErrorCode::Syntax:     return "Syntax";
        case ErrorCode::Deadlock:   return "Deadlock";
        case ErrorCode::Transient:  return "Transient";
        case ErrorCode::Mapping:    return "Mapping";
        case ErrorCode::Pool:       return "Pool";
        case ErrorCode::Unknown:    return "Unknown";
    }
    return "Unknown";
}

void throw_error(const Error& err) {
    switch (err.code) {
        case ErrorCode::Connection: throw ConnectionException(err);
        case ErrorCode::Timeout:    throw TimeoutException(err);
        case ErrorCode::Constraint: throw ConstraintException(err);
        case ErrorCode::Deadlock:   throw TransientException(err);
        case ErrorCode::Transient:  throw TransientException(err);
        case ErrorCode::Syntax:     throw QueryException(err);
        case ErrorCode::Mapping:    throw QueryException(err);
        case ErrorCode::Pool:       throw Exception(err);
        case ErrorCode::Ok:
        case ErrorCode::Unknown:    throw Exception(err);
    }
    throw Exception(err);
}

}  // namespace halcyon
```

- [x] **Step 5: Wire into the build**

In `CMakeLists.txt`, change the library sources to:

```cmake
add_library(halcyon
    src/core/version.cpp
    src/core/error.cpp)
```

In `tests/CMakeLists.txt`, change the test sources to:

```cmake
add_executable(halcyon_unit_tests
    unit/test_version.cpp
    unit/test_error.cpp)
```

- [x] **Step 6: Build and run to verify pass**

Run:

```bash
cmake --build build -j
ctest --test-dir build -R ErrorModel --output-on-failure
```

Expected: all three `ErrorModel.*` tests PASS.

- [x] **Step 7: Commit**

```bash
git add include/halcyon/error.hpp src/core/error.cpp CMakeLists.txt \
        tests/CMakeLists.txt tests/unit/test_error.cpp
git commit -m "feat: add Error model with ErrorCode and exception hierarchy"
```

---

## Task 3: Result<T> and Result<void>

**Files:**
- Create: `include/halcyon/result.hpp`
- Modify: `tests/CMakeLists.txt` (add `unit/test_result.cpp`)
- Test: `tests/unit/test_result.cpp`

- [x] **Step 1: Write the failing test**

Create `tests/unit/test_result.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "halcyon/result.hpp"

using halcyon::Error;
using halcyon::ErrorCode;
using halcyon::Result;

static Error makeError() {
    Error e;
    e.code = ErrorCode::Syntax;
    e.message = "bad sql";
    return e;
}

TEST(ResultT, HoldsValue) {
    Result<int> r(42);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r.value(), 42);
}

TEST(ResultT, HoldsError) {
    Result<int> r(makeError());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Syntax);
}

TEST(ResultT, ValueThrowsOnError) {
    Result<int> r(makeError());
    EXPECT_THROW(r.value(), halcyon::QueryException);
}

TEST(ResultT, ValueOrReturnsFallbackOnError) {
    Result<int> r(makeError());
    EXPECT_EQ(r.value_or(7), 7);
    Result<int> ok(3);
    EXPECT_EQ(ok.value_or(7), 3);
}

TEST(ResultT, MapTransformsValueAndPropagatesError) {
    Result<int> r(21);
    Result<int> doubled = r.map([](int v) { return v * 2; });
    ASSERT_TRUE(doubled.ok());
    EXPECT_EQ(doubled.value(), 42);

    Result<int> err(makeError());
    Result<int> mapped = err.map([](int v) { return v * 2; });
    ASSERT_FALSE(mapped.ok());
    EXPECT_EQ(mapped.error().code, ErrorCode::Syntax);
}

TEST(ResultT, AndThenChains) {
    Result<int> r(10);
    Result<std::string> s =
        r.and_then([](int v) { return Result<std::string>(std::to_string(v)); });
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s.value(), "10");
}

TEST(ResultVoid, OkAndError) {
    Result<void> ok;
    EXPECT_TRUE(ok.ok());
    Result<void> err(makeError());
    EXPECT_FALSE(err.ok());
    EXPECT_THROW(err.value(), halcyon::QueryException);
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build -R Result --output-on-failure` (after adding the file
and reconfiguring). Expected: compile FAIL — `halcyon/result.hpp` not found.

- [x] **Step 3: Write the Result header**

Create `include/halcyon/result.hpp`:

```cpp
#pragma once

#include <type_traits>
#include <utility>
#include <variant>

#include "halcyon/error.hpp"

namespace halcyon {

template <class T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}      // NOLINT(google-explicit-constructor)
    Result(Error err) : data_(std::move(err)) {}      // NOLINT(google-explicit-constructor)

    bool ok() const noexcept { return std::holds_alternative<T>(data_); }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & {
        ensure_ok();
        return std::get<T>(data_);
    }
    const T& value() const& {
        ensure_ok();
        return std::get<T>(data_);
    }
    T&& value() && {
        ensure_ok();
        return std::get<T>(std::move(data_));
    }

    const Error& error() const& { return std::get<Error>(data_); }

    T value_or(T fallback) const& { return ok() ? std::get<T>(data_) : fallback; }

    template <class F>
    auto map(F&& f) const& -> Result<std::decay_t<std::invoke_result_t<F, const T&>>> {
        using U = std::decay_t<std::invoke_result_t<F, const T&>>;
        if (ok()) return Result<U>(std::forward<F>(f)(std::get<T>(data_)));
        return Result<U>(std::get<Error>(data_));
    }

    template <class F>
    auto and_then(F&& f) const& -> std::invoke_result_t<F, const T&> {
        using R = std::invoke_result_t<F, const T&>;
        if (ok()) return std::forward<F>(f)(std::get<T>(data_));
        return R(std::get<Error>(data_));
    }

private:
    void ensure_ok() const {
        if (!ok()) throw_error(std::get<Error>(data_));
    }
    std::variant<T, Error> data_;
};

template <>
class Result<void> {
public:
    Result() : error_(), ok_(true) {}
    Result(Error err) : error_(std::move(err)), ok_(false) {}  // NOLINT

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    void value() const {
        if (!ok_) throw_error(error_);
    }

    const Error& error() const& { return error_; }

private:
    Error error_;
    bool ok_;
};

}  // namespace halcyon
```

- [x] **Step 4: Wire into the build**

In `tests/CMakeLists.txt`, add `unit/test_result.cpp` to `halcyon_unit_tests`:

```cmake
add_executable(halcyon_unit_tests
    unit/test_version.cpp
    unit/test_error.cpp
    unit/test_result.cpp)
```

- [x] **Step 5: Build and run to verify pass**

Run:

```bash
cmake --build build -j
ctest --test-dir build -R "Result" --output-on-failure
```

Expected: all `ResultT.*` and `ResultVoid.*` tests PASS.

- [x] **Step 6: Commit**

```bash
git add include/halcyon/result.hpp tests/CMakeLists.txt tests/unit/test_result.cpp
git commit -m "feat: add Result<T> and Result<void> with monadic helpers"
```

---

## Task 4: CLI driver seam + mock

**Files:**
- Create: `include/halcyon/detail/cli/driver.hpp`
- Create: `tests/unit/mock_cli_driver.hpp`
- Modify: `tests/CMakeLists.txt` (add `unit/test_cli_seam.cpp`)
- Test: `tests/unit/test_cli_seam.cpp`

- [x] **Step 1: Write the seam interface header**

Create `include/halcyon/detail/cli/driver.hpp`. This is the abstraction the real
`Db2CliDriver` (Plan 2) and `MockCliDriver` (tests) both implement. `ConnectionHandle`
is an opaque token so no `sqlcli1.h` types leak above the seam.

```cpp
#pragma once

#include <cstdint>
#include <string>

#include "halcyon/result.hpp"

namespace halcyon::detail::cli {

// Opaque handle to a physical connection owned by a driver implementation.
// 0 is reserved as the invalid handle.
enum class ConnectionHandle : std::uint64_t { invalid = 0 };

struct ConnectionParams {
    std::string connectionString;
};

// Thin seam over a Db2 CLI driver. The only interface the core/pool depend on
// for establishing and checking physical connections. Implementations must be
// safe to call from the thread that currently owns a given handle.
class ICliDriver {
public:
    virtual ~ICliDriver() = default;

    // Establishes a physical connection. On success returns a non-invalid handle.
    virtual Result<ConnectionHandle> connect(const ConnectionParams& params) = 0;

    // Releases a previously returned handle. Idempotent for already-closed handles.
    virtual Result<void> disconnect(ConnectionHandle handle) = 0;

    // Lightweight liveness probe (e.g. validation query) for the handle.
    virtual Result<bool> isAlive(ConnectionHandle handle) = 0;
};

}  // namespace halcyon::detail::cli
```

- [x] **Step 2: Write the mock driver**

Create `tests/unit/mock_cli_driver.hpp`. A scriptable fake: queue outcomes and
record calls so pool/core logic (Plans 2–3) can be tested without a database.

```cpp
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"

namespace halcyon::testing {

class MockCliDriver final : public detail::cli::ICliDriver {
public:
    using ConnectionHandle = detail::cli::ConnectionHandle;
    using ConnectionParams = detail::cli::ConnectionParams;

    // If non-empty, the next connect() pops and returns this error instead.
    std::deque<Error> connectErrors;
    // Controls what isAlive() reports for the next call(s); defaults to true.
    std::deque<bool> aliveResults;

    int connectCalls = 0;
    int disconnectCalls = 0;
    int aliveCalls = 0;
    std::vector<ConnectionParams> connectParams;

    Result<ConnectionHandle> connect(const ConnectionParams& params) override {
        ++connectCalls;
        connectParams.push_back(params);
        if (!connectErrors.empty()) {
            Error e = connectErrors.front();
            connectErrors.pop_front();
            return Result<ConnectionHandle>(e);
        }
        return Result<ConnectionHandle>(
            static_cast<ConnectionHandle>(++nextHandle_));
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

private:
    std::uint64_t nextHandle_ = 0;
};

}  // namespace halcyon::testing
```

- [x] **Step 3: Write the failing seam test**

Create `tests/unit/test_cli_seam.cpp`:

```cpp
#include <gtest/gtest.h>

#include "halcyon/detail/cli/driver.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Error;
using halcyon::ErrorCode;
using halcyon::detail::cli::ConnectionHandle;
using halcyon::detail::cli::ConnectionParams;
using halcyon::testing::MockCliDriver;

TEST(CliSeam, ConnectReturnsValidHandle) {
    MockCliDriver driver;
    auto r = driver.connect(ConnectionParams{"DATABASE=SAMPLE;"});
    ASSERT_TRUE(r.ok());
    EXPECT_NE(r.value(), ConnectionHandle::invalid);
    EXPECT_EQ(driver.connectCalls, 1);
    ASSERT_EQ(driver.connectParams.size(), 1u);
    EXPECT_EQ(driver.connectParams[0].connectionString, "DATABASE=SAMPLE;");
}

TEST(CliSeam, ConnectPropagatesScriptedError) {
    MockCliDriver driver;
    Error e;
    e.code = ErrorCode::Connection;
    e.sqlstate = "08001";
    e.retriable = true;
    driver.connectErrors.push_back(e);

    auto r = driver.connect(ConnectionParams{"bad"});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Connection);
}

TEST(CliSeam, IsAliveHonorsScriptedResults) {
    MockCliDriver driver;
    driver.aliveResults = {false, true};
    auto h = driver.connect(ConnectionParams{"x"}).value();
    EXPECT_FALSE(driver.isAlive(h).value());
    EXPECT_TRUE(driver.isAlive(h).value());
    EXPECT_EQ(driver.aliveCalls, 2);
}

TEST(CliSeam, DisconnectCounts) {
    MockCliDriver driver;
    auto h = driver.connect(ConnectionParams{"x"}).value();
    ASSERT_TRUE(driver.disconnect(h).ok());
    EXPECT_EQ(driver.disconnectCalls, 1);
}
```

- [x] **Step 4: Run test to verify it fails**

Run: `ctest --test-dir build -R CliSeam --output-on-failure` (after adding the file
and reconfiguring). Expected: compile FAIL — `driver.hpp`/`mock_cli_driver.hpp` not found.

- [x] **Step 5: Wire into the build**

In `tests/CMakeLists.txt`, add `unit/test_cli_seam.cpp`:

```cmake
add_executable(halcyon_unit_tests
    unit/test_version.cpp
    unit/test_error.cpp
    unit/test_result.cpp
    unit/test_cli_seam.cpp)
```

(The `target_include_directories(... unit)` line added in Task 1 makes
`mock_cli_driver.hpp` includable as `"mock_cli_driver.hpp"`.)

- [x] **Step 6: Build and run to verify pass**

Run:

```bash
cmake --build build -j
ctest --test-dir build -R CliSeam --output-on-failure
```

Expected: all four `CliSeam.*` tests PASS.

- [x] **Step 7: Run the full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: every test from Tasks 1–4 PASSES.

- [x] **Step 8: Commit**

```bash
git add include/halcyon/detail/cli/driver.hpp tests/unit/mock_cli_driver.hpp \
        tests/CMakeLists.txt tests/unit/test_cli_seam.cpp
git commit -m "feat: add ICliDriver seam and MockCliDriver for unit testing"
```

---

## Self-Review

**Spec coverage (Plan 1 scope):**
- Build system (CMake + find_package, options, vendored `third_party/clidriver`, RPATH) → Task 1. ✔
- `Result<T>` + monadic helpers + dual throwing bridge → Task 3. ✔
- `Error`/`ErrorCode`/exception hierarchy → Task 2. ✔
- Mockable CLI seam (`ICliDriver` + `MockCliDriver`) enabling unit tests → Task 4. ✔
- Unit test harness (GoogleTest) → Task 1. ✔
- Deferred to later plans (intentionally not in Plan 1): real `Db2CliDriver`,
  connection/statement/resultset, type binding, params, pool, async, facade,
  transactions, bulk, observability, integration tests.

**Placeholder scan:** No TBD/TODO/"handle errors"/vague steps; every code step
contains complete content. ✔

**Type consistency:** `Error`/`ErrorCode`/`throw_error` defined in Task 2 are used
unchanged in Tasks 3–4. `Result<T>`/`Result<void>` signatures in Task 3 match their
use in the seam (`Result<ConnectionHandle>`, `Result<void>`, `Result<bool>`) in
Task 4. `ConnectionHandle::invalid` defined and used consistently. `throw_error`
maps `Syntax`/`Mapping`→`QueryException`, matching `test_result.cpp` expectations. ✔

---

## Roadmap: subsequent plans

Each will be written as its own bite-sized TDD plan when we reach it.

- **Plan 2 — Core data path:** real `Db2CliDriver` (implements `ICliDriver` over
  `sqlcli1.h`, links `DB2::CLI`), `Connection`/`Statement`/`ResultSet`/`Row`,
  `TypeBinder<T>` mapping table, named/anonymous parameter binding, `row.as<...>()`,
  `HALCYON_REFLECT` + `queryAs<T>`, SQLSTATE→`ErrorCode` classification, and the
  first Dockerized Db2 integration tests (CTest label `integration`).
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
