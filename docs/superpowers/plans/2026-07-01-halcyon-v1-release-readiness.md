# Halcyon v1.0 Source-Consumption Release Readiness — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Halcyon cleanly consumable from source via both `find_package` and `FetchContent`, cut a real `1.0.0`, and publish a MkDocs+Doxygen docs site — adding no library features and no package-registry submission.

**Architecture:** The CMake `project(... VERSION)` becomes the single source of version truth (header generated via `configure_file`). A top-level-project guard makes developer-only targets default off when Halcyon is consumed as a subdirectory, so `FetchContent` builds only the library. Docs combine the existing markdown guide (MkDocs Material) with a Doxygen API reference published under `/api/`. CI gains FetchContent, macOS, and docs-build/deploy jobs.

**Tech Stack:** C++17, CMake (≥3.20), GoogleTest, Doxygen, MkDocs Material, GitHub Actions, GitHub Pages.

**Spec:** `docs/superpowers/specs/2026-07-01-halcyon-v1-release-readiness-design.md`

## Global Constraints

- **C++ standard:** C++17 only. No new C++20 features.
- **CMake floor:** keep `cmake_minimum_required(VERSION 3.20)`. Do **not** raise it.
- **Version single-source:** the only hand-edited version is `project(Halcyon VERSION ...)` in the root `CMakeLists.txt`. Everything else derives from it.
- **No new library features** and **no package-registry submission** (no vcpkg port / Conan recipe).
- **IBM `clidriver` is user-supplied** in every mode; consumers set `DB2_CLIDRIVER_ROOT`. Never commit anything under `third_party/`.
- **Seam invariant:** only `src/detail/cli/` may include `sqlcli1.h`. (Unchanged here, but do not violate it.)
- **Warnings:** `-Wall -Wextra -Wpedantic` clean; CI builds with `-Werror`.
- **Commits:** Conventional Commits (`feat:`, `fix:`, `build:`, `docs:`, `chore:`, `test:`, `ci:`). Small and focused.
- **macOS CI** uses the newest available **Intel** runner (`macos-26-intel`) so the x86_64 IBM driver links and loads.

---

## File structure

| File | Responsibility | Change |
|------|----------------|--------|
| `cmake/version.hpp.in` | Version header template (numeric + string) | Create |
| `include/halcyon/version.hpp` | Old hand-maintained header | Delete (replaced by generated) |
| `src/core/version.cpp` | `version()` impl | Return generated `version_string` |
| `CMakeLists.txt` | Build, version bump, configure_file, top-level guard, install generated header | Modify |
| `tests/unit/test_version.cpp` | Version unit test | Expect `1.0.0` / `1,0,0` |
| `tests/smoke/main.cpp` | Install smoke consumer | Compare against passed-in expected version |
| `tests/smoke/CMakeLists.txt` | Smoke consumer build | Accept + define expected version |
| `cmake/run_smoke_test.cmake` | Smoke driver | Forward expected version |
| `tests/consumer-fetchcontent/CMakeLists.txt` | FetchContent consumer project (NEW) | Create |
| `tests/consumer-fetchcontent/main.cpp` | FetchContent consumer source (NEW) | Create |
| `cmake/run_fetchcontent_test.cmake` | FetchContent test driver (NEW) | Create |
| `README.md` | Versioning policy + FetchContent/find_package snippets | Modify |
| `CHANGELOG.md` | Release changelog (NEW) | Create |
| `docs/RELEASING.md` | Release procedure (NEW) | Create |
| `mkdocs.yml` | MkDocs site config (NEW) | Create |
| `docs/requirements.txt` | Docs build deps (NEW) | Create |
| `docs/index.md` | Docs site landing page (NEW) | Create |
| `Doxyfile` | Doxygen config (NEW) | Create |
| `docs/build.sh` | One-command site build (NEW) | Create |
| `docs/README.md` | How to build docs (NEW) | Create |
| `include/halcyon/*.hpp` | Light Doxygen doc-comment pass on the public surface | Modify |
| `.gitignore` | Ignore `site/` and `docs/api/` | Modify |
| `.github/actions/setup-db2-clidriver/action.yml` | Add macOS driver support | Modify |
| `.github/workflows/ci.yml` | Add fetchcontent, macOS, docs jobs | Modify |

---

## Task 1: Single-source the version and bump to 1.0.0

Make the CMake project version the only hand-edited version, generate the header from it, and bump to `1.0.0`. TDD: the version unit test is the failing spec.

**Files:**
- Create: `cmake/version.hpp.in`
- Delete: `include/halcyon/version.hpp`
- Modify: `src/core/version.cpp`, `CMakeLists.txt`, `tests/unit/test_version.cpp`, `tests/smoke/main.cpp`, `tests/smoke/CMakeLists.txt`, `cmake/run_smoke_test.cmake`

**Interfaces:**
- Produces: generated header `<halcyon/version.hpp>` exposing `halcyon::version_major/minor/patch` (ints), `halcyon::version_string` (`constexpr std::string_view`), and `std::string_view halcyon::version() noexcept`.

- [ ] **Step 1: Update the version unit test to expect 1.0.0 (failing spec)**

Replace the contents of `tests/unit/test_version.cpp` with:

```cpp
#include <gtest/gtest.h>

#include "halcyon/version.hpp"

TEST(Version, ReturnsSemanticVersionString) {
    EXPECT_EQ(halcyon::version(), "1.0.0");
}

TEST(Version, ConstantsMatchString) {
    EXPECT_EQ(halcyon::version_major, 1);
    EXPECT_EQ(halcyon::version_minor, 0);
    EXPECT_EQ(halcyon::version_patch, 0);
}

TEST(Version, StringConstantMatchesFunction) {
    EXPECT_EQ(halcyon::version(), halcyon::version_string);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `ctest --test-dir build -R Version --output-on-failure`
Expected: FAIL — `version()` returns `"0.1.0"`, constants are `0,1,0`, and `version_string` does not exist (compile error). (If `build/` is stale, configure first: `cmake -S . -B build -DHALCYON_BUILD_TESTS=ON`.)

- [ ] **Step 3: Create the version header template**

Create `cmake/version.hpp.in`:

```cpp
#pragma once

#include <string_view>

namespace halcyon {

inline constexpr int version_major = @PROJECT_VERSION_MAJOR@;
inline constexpr int version_minor = @PROJECT_VERSION_MINOR@;
inline constexpr int version_patch = @PROJECT_VERSION_PATCH@;

// Full semantic version string, e.g. "1.0.0". Single-sourced from the CMake
// project() version via configure_file.
inline constexpr std::string_view version_string = "@PROJECT_VERSION@";

// Returns the semantic version string, e.g. "1.0.0".
std::string_view version() noexcept;

}  // namespace halcyon
```

- [ ] **Step 4: Delete the hand-maintained header**

Run: `git rm include/halcyon/version.hpp`

- [ ] **Step 5: Make version.cpp return the generated constant**

Replace the contents of `src/core/version.cpp` with:

```cpp
#include "halcyon/version.hpp"

namespace halcyon {

std::string_view version() noexcept {
    return version_string;
}

}  // namespace halcyon
```

- [ ] **Step 6: Bump the project version and wire configure_file**

In `CMakeLists.txt`, change line 2 to:

```cmake
project(Halcyon VERSION 1.0.0 LANGUAGES CXX)
```

After the `project(...)` call (e.g. immediately before the `option(...)` block, after line 7), add:

```cmake
# Single-source the version: generate include/halcyon/version.hpp from the CMake
# project version into the build tree. This generated header is the only one the
# compiler sees (the hand-maintained source header was removed).
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/halcyon/version.hpp"
    @ONLY)
```

In the `target_include_directories(halcyon PUBLIC ...)` block (lines 44-47), add the generated include dir so both in-tree builds and the library itself resolve the generated header:

```cmake
target_include_directories(halcyon
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>
        $<INSTALL_INTERFACE:include>)
```

In the install section, after the existing `install(DIRECTORY include/halcyon ...)` (line 118-120), install the generated header to the same location:

```cmake
# The generated version header lives in the build tree; install it alongside the
# hand-written public headers.
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/halcyon/version.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/halcyon)
```

- [ ] **Step 7: Thread the expected version through the smoke test**

In `cmake/run_smoke_test.cmake`, add `HALCYON_EXPECTED_VERSION` to the consumer-configure step (after the `DB2_CLIDRIVER_ROOT` line, ~line 41):

```cmake
        "-DDB2_CLIDRIVER_ROOT=${DB2_CLIDRIVER_ROOT}"
        "-DHALCYON_EXPECTED_VERSION=${HALCYON_EXPECTED_VERSION}")
```

In `CMakeLists.txt`, pass it into the `add_test(NAME halcyon_install_smoke ...)` command (after the `-DDB2CLI_LIBRARY_DIR=...` line, ~line 158):

```cmake
                -DHALCYON_EXPECTED_VERSION=${PROJECT_VERSION}
```

In `tests/smoke/CMakeLists.txt`, after the `add_executable`/`target_link_libraries` lines, define the expected version for the consumer:

```cmake
if(HALCYON_EXPECTED_VERSION)
    target_compile_definitions(halcyon_smoke
        PRIVATE HALCYON_EXPECTED_VERSION="${HALCYON_EXPECTED_VERSION}")
endif()
```

In `tests/smoke/main.cpp`, replace the final `return` line (`return v == "0.1.0" ? 0 : 1;`) with:

```cpp
#ifdef HALCYON_EXPECTED_VERSION
    return v == HALCYON_EXPECTED_VERSION ? 0 : 3;
#else
    return v.empty() ? 4 : 0;
#endif
```

- [ ] **Step 8: Reconfigure, rebuild, and run the version + smoke tests**

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build -R 'Version|halcyon_install_smoke' --output-on-failure
```
Expected: PASS. `Version.*` green; `halcyon_install_smoke` green (consumer reports `1.0.0`).

- [ ] **Step 9: Verify no stray version literal remains**

Run: `grep -rndE '0\.1\.0|version_(major|minor|patch) = [0-9]' --include=*.cpp --include=*.hpp --include=*.in --include=CMakeLists.txt include src tests cmake CMakeLists.txt`
Expected: the only matches are the generated-template placeholders in `cmake/version.hpp.in`. No literal `0.1.0` anywhere; no hand-set numeric version constants outside the template.

- [ ] **Step 10: Commit**

```bash
git add cmake/version.hpp.in src/core/version.cpp CMakeLists.txt \
        tests/unit/test_version.cpp tests/smoke/main.cpp \
        tests/smoke/CMakeLists.txt cmake/run_smoke_test.cmake
git rm --cached include/halcyon/version.hpp 2>/dev/null || true
git commit -m "build: single-source the version from CMake and bump to 1.0.0"
```

---

## Task 2: FetchContent/subdirectory consumption support

Add the top-level guard so developer-only targets default off when Halcyon is added via `add_subdirectory`/`FetchContent`, and prove it with a consumption test that fails without the guard.

**Files:**
- Modify: `CMakeLists.txt`
- Create: `tests/consumer-fetchcontent/CMakeLists.txt`, `tests/consumer-fetchcontent/main.cpp`, `cmake/run_fetchcontent_test.cmake`
- Modify: `CMakeLists.txt` (register the CTest case)

**Interfaces:**
- Consumes: the `halcyon::halcyon` alias target and `DB2::CLI` propagation from the root build (Task 1 unchanged them).
- Produces: CTest case `halcyon_fetchcontent_smoke`; CMake variable `PROJECT_IS_TOP_LEVEL` defined for CMake < 3.21.

- [ ] **Step 1: Write the FetchContent consumer source**

Create `tests/consumer-fetchcontent/main.cpp`:

```cpp
// Consumer that obtains Halcyon via FetchContent (SOURCE_DIR -> the repo) and
// links halcyon::halcyon. No live DB: construct (do not connect) the real CLI
// driver to prove DB2::CLI propagated through add_subdirectory, and report the
// version.
#include <iostream>
#include <string>

#include <halcyon/halcyon.hpp>

int main() {
    const std::string v{halcyon::version()};
    std::cout << "halcyon (fetchcontent) " << v << "\n";

    auto driver = halcyon::detail::cli::make_db2_cli_driver();
    if (!driver) return 2;

    return v.empty() ? 3 : 0;
}
```

- [ ] **Step 2: Write the FetchContent consumer project**

Create `tests/consumer-fetchcontent/CMakeLists.txt`:

```cmake
# Standalone consumer that pulls Halcyon via FetchContent (pointed at the local
# source tree, so no network) and links halcyon::halcyon. Proves subdirectory
# consumption: the library target + usage requirements are available, and
# Halcyon's developer-only targets are NOT created in the consumer build.
cmake_minimum_required(VERSION 3.20)
project(halcyon_fetchcontent_consumer CXX)

if(NOT HALCYON_SOURCE_DIR)
    message(FATAL_ERROR "HALCYON_SOURCE_DIR is required")
endif()

include(FetchContent)
FetchContent_Declare(Halcyon SOURCE_DIR "${HALCYON_SOURCE_DIR}")
FetchContent_MakeAvailable(Halcyon)

# The guard must keep Halcyon's tests out of a consumer build.
if(TARGET halcyon_unit_tests)
    message(FATAL_ERROR
        "consumer build created Halcyon's test target; top-level guard missing")
endif()

add_executable(halcyon_fc_consumer main.cpp)
target_link_libraries(halcyon_fc_consumer PRIVATE halcyon::halcyon)

if(DB2CLI_LIBRARY_DIR)
    set_target_properties(halcyon_fc_consumer PROPERTIES
        BUILD_RPATH "${DB2CLI_LIBRARY_DIR}")
endif()
```

- [ ] **Step 3: Write the FetchContent test driver**

Create `cmake/run_fetchcontent_test.cmake`:

```cmake
# FetchContent/add_subdirectory consumption test, run via `cmake -P`. Configures,
# builds, and runs a consumer that FetchContent_MakeAvailable(Halcyon) against the
# local source tree. Any failed step fails the test.
#
# Required -D vars: HALCYON_SOURCE_DIR, HALCYON_BUILD_DIR, DB2_CLIDRIVER_ROOT.

if(NOT HALCYON_SOURCE_DIR OR NOT HALCYON_BUILD_DIR)
    message(FATAL_ERROR "HALCYON_SOURCE_DIR and HALCYON_BUILD_DIR are required")
endif()

set(consumer_build "${HALCYON_BUILD_DIR}/fetchcontent/consumer")
file(REMOVE_RECURSE "${consumer_build}")

function(run_step desc)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "fetchcontent step failed (${desc}): exit ${rc}")
    endif()
endfunction()

set(config_args "")
if(SMOKE_CONFIG)
    set(config_args --config "${SMOKE_CONFIG}")
endif()

run_step("consumer configure"
    ${CMAKE_COMMAND}
        -S "${HALCYON_SOURCE_DIR}/tests/consumer-fetchcontent"
        -B "${consumer_build}"
        "-DCMAKE_BUILD_TYPE=${SMOKE_CONFIG}"
        "-DHALCYON_SOURCE_DIR=${HALCYON_SOURCE_DIR}"
        "-DDB2_CLIDRIVER_ROOT=${DB2_CLIDRIVER_ROOT}")

run_step("consumer build"
    ${CMAKE_COMMAND} --build "${consumer_build}" ${config_args})

set(exe "${consumer_build}/halcyon_fc_consumer")
if(NOT EXISTS "${exe}" AND SMOKE_CONFIG)
    set(exe "${consumer_build}/${SMOKE_CONFIG}/halcyon_fc_consumer")
endif()
run_step("consumer run" "${exe}")

message(STATUS "halcyon_fetchcontent_smoke: OK")
```

- [ ] **Step 4: Register the CTest case (before adding the guard, so it fails)**

In `CMakeLists.txt`, inside the `if(HALCYON_BUILD_SMOKE_TEST)` block (after the existing `add_test(NAME halcyon_install_smoke ...)`, ~line 161), add:

```cmake
        add_test(NAME halcyon_fetchcontent_smoke
            COMMAND ${CMAKE_COMMAND}
                -DHALCYON_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DHALCYON_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -DDB2_CLIDRIVER_ROOT=${DB2_CLIDRIVER_ROOT}
                -DSMOKE_CONFIG=$<CONFIG>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_fetchcontent_test.cmake)
```

- [ ] **Step 5: Run the new test to verify it FAILS without the guard**

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON
ctest --test-dir build -R halcyon_fetchcontent_smoke --output-on-failure
```
Expected: FAIL at "consumer configure" — with `HALCYON_BUILD_TESTS` defaulting `ON`, the consumer's `FetchContent_MakeAvailable(Halcyon)` enters Halcyon's `tests` subtree, creates `halcyon_unit_tests`, and the `if(TARGET halcyon_unit_tests)` guard in the consumer `FATAL_ERROR`s.

- [ ] **Step 6: Add the top-level guard and default developer options off in subdir mode**

In `CMakeLists.txt`, immediately after the `set(CMAKE_EXPORT_COMPILE_COMMANDS ON)` line (line 7) and before the `option(...)` block, add:

```cmake
# Define PROJECT_IS_TOP_LEVEL for CMake < 3.21 so we can keep the 3.20 floor.
# When Halcyon is consumed via add_subdirectory/FetchContent it is NOT top-level,
# and developer-only targets (tests, examples) must default OFF.
if(NOT DEFINED PROJECT_IS_TOP_LEVEL)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(PROJECT_IS_TOP_LEVEL TRUE)
    else()
        set(PROJECT_IS_TOP_LEVEL FALSE)
    endif()
endif()
```

Then change the `HALCYON_BUILD_TESTS` option default (line 15) from `ON` to the guard:

```cmake
option(HALCYON_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
```

(`HALCYON_BUILD_EXAMPLES`, `HALCYON_BUILD_INTEGRATION_TESTS`, and `HALCYON_BUILD_STRESS_TESTS` already default `OFF`, which is correct for consumers.)

- [ ] **Step 7: Run the FetchContent and install smoke tests to verify they PASS**

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build -R 'smoke' --output-on-failure
```
Expected: both `halcyon_install_smoke` and `halcyon_fetchcontent_smoke` PASS. The consumer build no longer creates `halcyon_unit_tests`.

- [ ] **Step 8: Confirm the top-level developer build is unchanged**

Run: `ctest --test-dir build -LE 'integration' --output-on-failure`
Expected: full unit + smoke suite PASS (tests still build by default at top level).

- [ ] **Step 9: Commit**

```bash
git add CMakeLists.txt tests/consumer-fetchcontent cmake/run_fetchcontent_test.cmake
git commit -m "build: support FetchContent consumption with a top-level guard and test"
```

---

## Task 3: Release hygiene — CHANGELOG, compatibility policy, RELEASING

Documentation-only. Adds the changelog, the SemVer/compatibility statement, the consumer snippets, and the release procedure.

**Files:**
- Create: `CHANGELOG.md`, `docs/RELEASING.md`
- Modify: `README.md`

- [ ] **Step 1: Create the changelog**

Create `CHANGELOG.md`:

```markdown
# Changelog

All notable changes to Halcyon are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html) for its
public C++ API (see "Versioning & compatibility" in the README).

## [Unreleased]

## [1.0.0] - 2026-07-01

First stable release. Source-consumption ready via `find_package(Halcyon)` and
CMake `FetchContent`.

### Added
- High-level `Database` facade with OO/fluent and functional free-function styles.
- Dual error model: `Result<T>` and throwing overloads; `Error` with `ErrorCode`,
  SQLSTATE, native SQLCODE, and a `retriable` flag.
- Named (`:name`) and anonymous (`?`) parameters; tuple and `HALCYON_REFLECT`
  struct row mapping; forward-only streaming `ResultSet`.
- Thread-safe `ConnectionPool` with validation, idle/lifetime reaping,
  transparent reconnect, and safe auto-retry of idempotent operations.
- Per-connection prepared-statement LRU cache.
- `std::future`-based async (`queryAsync`/`executeAsync`); coroutine-ready
  executor seam.
- RAII transactions (`ScopedTransaction`) and functional `transaction(...)`.
- Bulk insert via true Db2 CLI column-wise array binding (`executeBatch`,
  `batchOf`), with byte-budget chunking and a `Transaction::executeBatch` overload.
- Optional Prometheus metrics and OpenTelemetry tracing (with W3C context
  propagation), behind interfaces and CMake toggles; zero overhead when disabled.
- CMake package: `find_package(Halcyon)` -> `halcyon::halcyon`, and `FetchContent`
  consumption.

[Unreleased]: https://github.com/<owner>/Halcyon/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/<owner>/Halcyon/releases/tag/v1.0.0
```

(Replace `<owner>` with the repository owner when the remote is known.)

- [ ] **Step 2: Add the versioning & compatibility section to the README**

In `README.md`, immediately before the `## License` section (line 282), insert:

```markdown
## Versioning & compatibility

Halcyon follows [Semantic Versioning](https://semver.org/) for its **public C++
API** — everything under `include/halcyon/` except `include/halcyon/detail/`.
Symbols under `detail::` are implementation details and may change in any release.

- **Source compatibility** is the guarantee: consume Halcyon from source
  (`find_package` against your own build, or `FetchContent`). No binary-ABI
  stability is promised across versions.
- The **IBM Db2 CLI driver (`clidriver`) is a user-supplied dependency** and is
  not redistributed by this project. Provide it via `DB2_CLIDRIVER_ROOT` (or the
  vendored `third_party/clidriver`) in every consumption mode.

See [`CHANGELOG.md`](CHANGELOG.md) for the per-release history.
```

- [ ] **Step 3: Add the FetchContent consumption snippet to the README**

In `README.md`, in the "Using Halcyon in your CMake project" section (line 168-173), replace the single `find_package` block with both modes:

```markdown
### Using Halcyon in your CMake project

**Installed (find_package):**

```cmake
find_package(Halcyon 1.0 REQUIRED)
target_link_libraries(my_app PRIVATE halcyon::halcyon)
```

**From source (FetchContent):**

```cmake
include(FetchContent)
FetchContent_Declare(Halcyon
    GIT_REPOSITORY https://github.com/<owner>/Halcyon.git
    GIT_TAG        v1.0.0)
FetchContent_MakeAvailable(Halcyon)
target_link_libraries(my_app PRIVATE halcyon::halcyon)
```

Both modes require you to supply the IBM Db2 CLI driver, e.g.:

```bash
cmake -S . -B build -DDB2_CLIDRIVER_ROOT=/path/to/clidriver
```
```

(Replace `<owner>` with the repository owner when the remote is known.)

- [ ] **Step 4: Create the release procedure**

Create `docs/RELEASING.md`:

```markdown
# Releasing Halcyon

Halcyon ships as source. A release is a git tag plus a GitHub Release; there is
no package-registry publish step.

## 1. Pre-flight verification

Run the full matrix locally (see `AGENTS.md` for details):

```bash
# Unit + install/FetchContent smoke (no DB)
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build -LE integration --output-on-failure

# Concurrency under ThreadSanitizer
cmake -S . -B build-tsan -DHALCYON_BUILD_TESTS=ON \
      -DHALCYON_BUILD_STRESS_TESTS=ON -DHALCYON_SANITIZER=thread
cmake --build build-tsan -j
ctest --test-dir build-tsan -L stress --output-on-failure

# Live Db2 integration (Docker) — see AGENTS.md "Integration tests"
# Docs build
docs/build.sh
```

All must pass with nothing skipped in the integration run.

## 2. Bump the version

Edit the single source — `project(Halcyon VERSION X.Y.Z ...)` in the root
`CMakeLists.txt`. The version header, smoke test, and package version file all
derive from it. Update `CHANGELOG.md`: move `[Unreleased]` entries into a new
`[X.Y.Z]` section with the date, and refresh the compare/links footnotes.

## 3. Tag and release

```bash
git commit -am "chore: release vX.Y.Z"
git tag -a vX.Y.Z -m "Halcyon vX.Y.Z"
git push origin main --tags
```

Create the GitHub Release from the tag, pasting the `CHANGELOG.md` section as the
notes. The `docs` CI job publishes the site for `main`/tags.

## 4. Compatibility check (major bumps)

Before a major bump, confirm public-API changes under `include/halcyon/`
(excluding `detail/`) are intended and documented in `CHANGELOG.md`.
```

- [ ] **Step 5: Verify links and render**

Run: `grep -n "<owner>" CHANGELOG.md README.md`
Expected: only the intentional `<owner>` placeholders (to be filled when the
remote is known). Visually confirm the README sections render (headers balanced,
code fences closed).

- [ ] **Step 6: Commit**

```bash
git add CHANGELOG.md docs/RELEASING.md README.md
git commit -m "docs: add changelog, compatibility policy, and release procedure"
```

---

## Task 4: Documentation site (MkDocs Material + Doxygen)

Build a docs site from the existing guide plus a Doxygen API reference, with a
single build script. Requires `mkdocs-material` and `doxygen` available locally.

**Files:**
- Create: `mkdocs.yml`, `docs/requirements.txt`, `docs/index.md`, `Doxyfile`, `docs/build.sh`, `docs/README.md`
- Modify: `include/halcyon/halcyon.hpp` and the principal public headers (light doc-comment pass), `.gitignore`

- [ ] **Step 1: Ignore generated docs output**

In `.gitignore`, under the build directories section, add:

```gitignore
# Generated documentation site
site/
docs/api/
```

- [ ] **Step 2: Pin the docs build dependencies**

Create `docs/requirements.txt`:

```text
mkdocs-material==9.5.39
```

- [ ] **Step 3: Create the docs landing page**

Create `docs/index.md`:

```markdown
# Halcyon

A modern **C++17** client library for **IBM Db2**, built on the IBM Db2 CLI.

- **[Guide](guide/index.md)** — narrative documentation: getting started,
  querying, pooling, transactions, batch, async, observability.
- **[API reference](api/index.html)** — generated from the public headers.

See the project [README](https://github.com/<owner>/Halcyon) and
[CHANGELOG](https://github.com/<owner>/Halcyon/blob/main/CHANGELOG.md).
```

- [ ] **Step 4: Create the MkDocs config**

Create `mkdocs.yml`:

```yaml
site_name: Halcyon
site_description: Modern C++17 client library for IBM Db2
docs_dir: docs
# docs/ also holds non-site material (specs, plans, build tooling) and the
# generated Doxygen tree. Exclude the former from the build entirely, and tell
# --strict that the Doxygen output is intentionally outside the nav.
exclude_docs: |
  superpowers/
  requirements.txt
  build.sh
  README.md
  RELEASING.md
not_in_nav: |
  api/**
theme:
  name: material
  features:
    - navigation.sections
    - navigation.top
    - content.code.copy
    - search.highlight
markdown_extensions:
  - admonition
  - toc:
      permalink: true
  - pymdownx.highlight
  - pymdownx.superfences
nav:
  - Home: index.md
  - Guide:
      - Overview: guide/index.md
      - Getting Started: guide/getting-started.md
      - Error Handling: guide/error-handling.md
      - Querying: guide/querying.md
      - Parameters & Types: guide/parameters-and-types.md
      - Connection Pool: guide/connection-pool.md
      - Transactions: guide/transactions.md
      - Batch Operations: guide/batch-operations.md
      - Async: guide/async.md
      - Observability: guide/observability.md
      - Advanced: guide/advanced.md
  - API Reference: api/index.html
```

- [ ] **Step 5: Create the Doxygen config**

Create `Doxyfile` (minimal, public-surface only; `EXTRACT_ALL` surfaces existing
`//` comments). Output goes under `docs/api` so MkDocs publishes it at `/api/`:

```text
PROJECT_NAME           = "Halcyon"
PROJECT_BRIEF          = "Modern C++17 client library for IBM Db2"
OUTPUT_DIRECTORY       = docs
HTML_OUTPUT            = api
GENERATE_LATEX         = NO
GENERATE_HTML          = YES
INPUT                  = include/halcyon
FILE_PATTERNS          = *.hpp
RECURSIVE              = YES
EXCLUDE                = include/halcyon/detail
EXTRACT_ALL            = YES
JAVADOC_AUTOBRIEF      = YES
QUIET                  = YES
WARN_IF_UNDOCUMENTED   = NO
GENERATE_TREEVIEW      = YES
```

- [ ] **Step 6: Create the build script**

Create `docs/build.sh`:

```bash
#!/usr/bin/env bash
# Build the full documentation site: Doxygen API reference under docs/api, then
# the MkDocs Material site into site/ (which includes docs/api as /api).
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==> Doxygen API reference"
doxygen Doxyfile

echo "==> MkDocs site"
mkdocs build --strict

echo "==> Site built in ./site (open ./site/index.html)"
```

Make it executable:

Run: `chmod +x docs/build.sh`

- [ ] **Step 7: Document the docs build**

Create `docs/README.md`:

```markdown
# Documentation

The published site combines the narrative guide (MkDocs Material, source in
`docs/guide/`) with a generated API reference (Doxygen, from `include/halcyon/`).

## Build locally

```bash
python -m pip install -r docs/requirements.txt   # mkdocs-material
# plus Doxygen, e.g. `brew install doxygen` or `apt-get install doxygen`
docs/build.sh
```

Output lands in `site/` (gitignored). `docs/build.sh` runs Doxygen first
(into `docs/api/`, also gitignored) and then `mkdocs build --strict`.
```

- [ ] **Step 8: Light Doxygen doc-comment pass on the public surface**

Add a brief `///`-style summary to the umbrella header and the principal public
types so the API reference reads well. This is a **targeted** pass, not an
exhaustive rewrite. Cover: `halcyon.hpp` (file brief), and the declarations of
`Database`, `Connection`, `Transaction`/`ScopedTransaction`, `ResultSet`/`Row`,
`Result<T>`, `Error`, `PoolConfig`, `Batch`/`batchOf`, and the async entry points.

Pattern — add a `\brief` line above the type/function. Example for
`include/halcyon/database.hpp` (above `class Database`):

```cpp
/// \brief High-level entry point: a pooled, thread-safe Db2 database handle.
///
/// Open with `Database::open` (returns `Result<Database>`) or
/// `Database::openOrThrow`. Provides query/execute/transaction/batch in both
/// `Result<T>` and throwing styles. Safe to share across threads.
class Database {
```

Example for `include/halcyon/result.hpp` (above `class Result`):

```cpp
/// \brief Holds either a value of type `T` or an `Error` (vendored, C++17).
///
/// `.value()` throws `Exception` on error; `.ok()`/`operator bool` test success.
/// Compose with `map`/`and_then`/`or_else`.
template <class T>
class Result {
```

Apply the same one-`\brief`-plus-short-paragraph treatment to each type listed
above, in its own header. Keep wording aligned with the existing guide.

- [ ] **Step 9: Build the site and verify output**

```bash
python -m pip install -r docs/requirements.txt
docs/build.sh
test -f site/index.html && test -f site/api/index.html && echo "SITE OK"
```
Expected: `mkdocs build --strict` succeeds with no warnings (broken links fail
`--strict`); `SITE OK` prints. If `--strict` flags a missing nav file or bad
link, fix the path and rebuild.

- [ ] **Step 10: Commit**

```bash
git add mkdocs.yml docs/requirements.txt docs/index.md docs/README.md \
        Doxyfile docs/build.sh .gitignore include/halcyon
git commit -m "docs: add MkDocs Material + Doxygen documentation site"
```

---

## Task 5: CI — FetchContent, macOS, and macOS driver support

Add a FetchContent consumption job and a macOS build-test job, and teach the
driver-setup action to fetch the macOS driver.

**Files:**
- Modify: `.github/actions/setup-db2-clidriver/action.yml`, `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: the `halcyon_fetchcontent_smoke` CTest case (Task 2) and the
  existing `setup-db2-clidriver` action.

- [ ] **Step 1: Add macOS support to the driver-setup action**

In `.github/actions/setup-db2-clidriver/action.yml`, add macOS inputs and an
OS-aware download. Add to `inputs:` (after the existing `sha256` input):

```yaml
  macos_url:
    description: macOS (x86_64) clidriver tarball URL.
    default: https://public.dhe.ibm.com/ibmdl/export/pub/software/data/db2/drivers/odbc_cli/macos64_odbc_cli.tar.gz
  macos_sha256:
    description: >
      Expected SHA256 of the macOS tarball. Like the linux pin, this is a moving
      target; when it fails, review the new driver and update this value.
    default: REPLACE_WITH_MACOS_SHA256
```

Replace the `Restore cached driver` + `Download, verify, extract` steps with
OS-aware variants:

```yaml
    - name: Select driver source
      id: src
      shell: bash
      run: |
        if [ "$RUNNER_OS" = "macOS" ]; then
          echo "url=${{ inputs.macos_url }}" >> "$GITHUB_OUTPUT"
          echo "sha256=${{ inputs.macos_sha256 }}" >> "$GITHUB_OUTPUT"
        else
          echo "url=${{ inputs.url }}" >> "$GITHUB_OUTPUT"
          echo "sha256=${{ inputs.sha256 }}" >> "$GITHUB_OUTPUT"
        fi

    - name: Restore cached driver
      id: cache
      uses: actions/cache@v4
      with:
        path: third_party/clidriver
        key: clidriver-${{ runner.os }}-${{ steps.src.outputs.sha256 }}

    - name: Download, verify, extract
      if: steps.cache.outputs.cache-hit != 'true'
      shell: bash
      run: |
        set -euo pipefail
        mkdir -p third_party
        for attempt in 1 2 3 4 5; do
          echo "Downloading Db2 CLI driver (attempt ${attempt}/5)"
          rm -f /tmp/clidriver.tar.gz
          if curl --connect-timeout 20 --max-time 120 -fSL "${{ steps.src.outputs.url }}" -o /tmp/clidriver.tar.gz; then
            break
          fi
          if [ "$attempt" -eq 5 ]; then
            echo "Failed to download Db2 CLI driver after ${attempt} attempts" >&2
            exit 1
          fi
          sleep $((attempt * 5))
        done
        if [ "$RUNNER_OS" = "macOS" ]; then
          echo "${{ steps.src.outputs.sha256 }}  /tmp/clidriver.tar.gz" | shasum -a 256 -c -
        else
          echo "${{ steps.src.outputs.sha256 }}  /tmp/clidriver.tar.gz" | sha256sum -c -
        fi
        tar -xzf /tmp/clidriver.tar.gz -C third_party
        test -f third_party/clidriver/include/sqlcli1.h
        # macOS: clear Gatekeeper quarantine so the driver dlopens (see AGENTS.md).
        if [ "$RUNNER_OS" = "macOS" ]; then
          chmod -R u+w third_party/clidriver
          xattr -r -d com.apple.quarantine third_party/clidriver || true
        fi
```

- [ ] **Step 2: Pin the macOS driver checksum**

The macOS default `REPLACE_WITH_MACOS_SHA256` must be a real pin. Fetch the
tarball once and compute it:

```bash
curl -fSL https://public.dhe.ibm.com/ibmdl/export/pub/software/data/db2/drivers/odbc_cli/macos64_odbc_cli.tar.gz -o /tmp/macos_cli.tar.gz
shasum -a 256 /tmp/macos_cli.tar.gz
```
Replace `REPLACE_WITH_MACOS_SHA256` in the action with the printed hash. (If
fetching is not possible in this environment, leave a `# TODO: pin` comment is
**not** acceptable — instead wire the value as a required workflow input until the
hash is known. Prefer pinning the real value.)

- [ ] **Step 3: Add the FetchContent CI job**

In `.github/workflows/ci.yml`, after the `build-test` job (line 101) add:

```yaml
  # ---------------------------------------------------------------------------
  # Consumption via FetchContent/add_subdirectory. No live DB.
  # ---------------------------------------------------------------------------
  fetchcontent:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4

      - name: Install toolchain
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends \
            cmake ninja-build g++-13

      - name: Set up Db2 CLI driver
        uses: ./.github/actions/setup-db2-clidriver

      - name: Configure
        env:
          CC: gcc-13
          CXX: g++-13
        run: cmake -S . -B build -G Ninja -DHALCYON_BUILD_TESTS=ON

      - name: Build
        run: cmake --build build -j

      - name: FetchContent + install smoke tests
        run: ctest --test-dir build -R smoke --output-on-failure
```

- [ ] **Step 4: Add the macOS build-test job**

In `.github/workflows/ci.yml`, after the `fetchcontent` job add:

```yaml
  # ---------------------------------------------------------------------------
  # macOS build + unit tests. Intel runner so the x86_64 IBM driver links and
  # loads. No live DB.
  # ---------------------------------------------------------------------------
  build-test-macos:
    runs-on: macos-26-intel
    steps:
      - uses: actions/checkout@v4

      - name: Install toolchain
        run: brew install cmake ninja

      - name: Set up Db2 CLI driver
        uses: ./.github/actions/setup-db2-clidriver

      - name: Configure
        run: |
          cmake -S . -B build -G Ninja \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DHALCYON_BUILD_TESTS=ON \
            -DHALCYON_WARNINGS_AS_ERRORS=ON

      - name: Build
        run: cmake --build build -j

      - name: Test (unit + smoke)
        run: ctest --test-dir build -LE integration --output-on-failure
```

- [ ] **Step 5: Validate the workflow syntax**

Run: `python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ci.yml')); yaml.safe_load(open('.github/actions/setup-db2-clidriver/action.yml')); print('YAML OK')"`
Expected: `YAML OK`. Visually confirm the new jobs match the indentation/style of
the existing jobs.

- [ ] **Step 6: Commit**

```bash
git add .github/actions/setup-db2-clidriver/action.yml .github/workflows/ci.yml
git commit -m "ci: add FetchContent and macOS build-test jobs"
```

---

## Task 6: CI — documentation build and GitHub Pages deploy

Build the docs on every PR (catching broken links) and deploy to GitHub Pages on
`main` and tags.

**Files:**
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: Add workflow-level Pages permissions**

In `.github/workflows/ci.yml`, after the `concurrency:` block (line 12) add a
top-level `permissions` block (least privilege; the deploy job needs Pages):

```yaml
permissions:
  contents: read
  pages: write
  id-token: write
```

- [ ] **Step 2: Add the docs build + deploy job**

At the end of `.github/workflows/ci.yml`, add:

```yaml
  # ---------------------------------------------------------------------------
  # Build the docs site on every run (mkdocs --strict catches broken links);
  # deploy to GitHub Pages only on main and tags.
  # ---------------------------------------------------------------------------
  docs:
    runs-on: ubuntu-24.04
    environment:
      name: github-pages
      url: ${{ steps.deployment.outputs.page_url }}
    steps:
      - uses: actions/checkout@v4

      - name: Install tooling
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends doxygen
          python -m pip install -r docs/requirements.txt

      - name: Build site (Doxygen + MkDocs --strict)
        run: docs/build.sh

      - name: Upload Pages artifact
        if: github.ref == 'refs/heads/main' || startsWith(github.ref, 'refs/tags/')
        uses: actions/upload-pages-artifact@v3
        with:
          path: site

      - name: Deploy to GitHub Pages
        id: deployment
        if: github.ref == 'refs/heads/main' || startsWith(github.ref, 'refs/tags/')
        uses: actions/deploy-pages@v4
```

- [ ] **Step 3: Validate the workflow syntax**

Run: `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/ci.yml')); print('YAML OK')"`
Expected: `YAML OK`.

- [ ] **Step 4: Note the one-time repo setting**

GitHub Pages must be set to "GitHub Actions" as its source in repository Settings
→ Pages for the deploy to publish. This is a one-time manual repo setting (not
code); record it in `docs/RELEASING.md` if not already obvious. Add this line to
the `docs/RELEASING.md` "Tag and release" section:

```markdown
> First-time setup: in repo Settings → Pages, set Source = "GitHub Actions" so
> the `docs` workflow job can publish the site.
```

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/ci.yml docs/RELEASING.md
git commit -m "ci: build docs on PRs and deploy to GitHub Pages on main/tags"
```

---

## Task 7: Final verification

Run the full release-readiness matrix and confirm everything is green. No code
changes; this is the gate before the branch is declared release-ready.

**Files:** none (verification only).

- [ ] **Step 1: Clean unit + smoke (both consumption modes)**

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build -LE integration --output-on-failure
```
Expected: all PASS, including `halcyon_install_smoke` and
`halcyon_fetchcontent_smoke`. Consumer reports `1.0.0`.

- [ ] **Step 2: TSan stress**

```bash
cmake -S . -B build-tsan -DHALCYON_BUILD_TESTS=ON \
      -DHALCYON_BUILD_STRESS_TESTS=ON -DHALCYON_SANITIZER=thread
cmake --build build-tsan -j
ctest --test-dir build-tsan -L stress --output-on-failure
```
Expected: PASS, no ThreadSanitizer reports.

- [ ] **Step 3: Live Db2 integration**

Follow `AGENTS.md` "Integration tests against live Db2": bring up the container,
set `HALCYON_TEST_DSN`, configure with `-DHALCYON_BUILD_INTEGRATION_TESTS=ON`,
and run `ctest -L integration`. Expected: all PASS, **none skipped**. Tear the
container down afterward.

- [ ] **Step 4: Docs build**

Run: `docs/build.sh && test -f site/index.html && test -f site/api/index.html && echo OK`
Expected: `mkdocs build --strict` clean; `OK`.

- [ ] **Step 5: Version single-source audit**

Run: `grep -rndE '0\.1\.0' include src tests cmake CMakeLists.txt docs/RELEASING.md`
Expected: no matches (the version is `1.0.0` and single-sourced).

- [ ] **Step 6: Use superpowers:requesting-code-review before merging the branch.**

---

## Self-review notes

- **Spec coverage:** A.1 version single-source → Task 1; A.2 bump → Task 1; A.3
  CHANGELOG → Task 3; A.4 compatibility policy → Task 3; A.5 RELEASING + notes →
  Task 3 (+ Pages note in Task 6). B.1 top-level guard → Task 2; B.2 target
  propagation → Task 2 (consumer links + driver construct); B.3 FetchContent test
  → Task 2; B.4 consumer docs → Task 3. C.1 MkDocs → Task 4; C.2 Doxygen + doc
  pass → Task 4; C.3 integration/build script → Task 4. D.1 fetchcontent job →
  Task 5; D.2 docs job → Task 6; D.3 macOS job (+ driver action) → Task 5. E
  non-goals respected (no registry, no features). Final verification → Task 7.
- **Type/name consistency:** generated header exposes `version_string`,
  `version_major/minor/patch`, `version()` — used consistently across Task 1.
  CTest cases named `halcyon_install_smoke` (existing) and
  `halcyon_fetchcontent_smoke` (Task 2, referenced in Task 5 `-R smoke`). Consumer
  target `halcyon_fc_consumer` and guard target `halcyon_unit_tests` match the
  real test target name in `tests/CMakeLists.txt`.
- **Known placeholders by design:** `<owner>` in CHANGELOG/README/docs (filled
  when the remote is known) and the macOS driver checksum (Task 5 Step 2 pins the
  real value).
