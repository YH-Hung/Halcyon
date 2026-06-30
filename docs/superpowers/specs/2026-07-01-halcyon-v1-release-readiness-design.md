# Halcyon v1.0 — Source-Consumption Release Readiness — Design Spec

**Date:** 2026-07-01
**Status:** Approved design (pre-implementation)

## 1. Overview

Halcyon's v1 feature set is complete and verified (core data path, pool/concurrency,
statement cache, array binding, observability, streaming reads). The library is,
however, still at version `0.1.0` and is only *partially* consumable by outside
projects: `find_package(Halcyon)` is supported and smoke-tested, but
`FetchContent`/`add_subdirectory` consumption is unsafe, there is no published
documentation site, and there is no release hygiene (no `CHANGELOG`, no version
single-source, no tags, no stated compatibility policy).

This project makes Halcyon cleanly consumable **from source** in both
`find_package` and `FetchContent` modes, cuts a real `1.0.0`, and publishes a
documentation site. It deliberately adds **no library features** and submits to
**no package registry**.

### Goals

- Cut a clean **`1.0.0`** from a single source of version truth.
- Make `FetchContent`/`add_subdirectory` consumption correct (no test/example
  bleed into the consumer build).
- Keep `find_package(Halcyon)` working exactly as today.
- Publish a documentation site combining the existing narrative guide (MkDocs
  Material) and a generated API reference (Doxygen).
- Broaden CI to cover both consumption modes, the docs build, and macOS.
- Establish release hygiene: `CHANGELOG.md`, a SemVer/compatibility policy, and a
  documented release procedure.

### Non-goals (explicitly out of scope)

- **Package-registry submission** — no vcpkg port, no Conan recipe, no Conan
  Center / vcpkg registry PRs.
- **New library features** — no savepoints, LOB streaming, coroutines, or new
  platforms (Windows/AIX).
- **ABI / symbol versioning** beyond the stated source-compatibility policy.
  Halcyon is a static, header-heavy library consumed from source; binary ABI
  stability is not promised.
- **Versioned documentation** (e.g. MkDocs `mike`) — a single "latest" site only.
- **Bundling the IBM Db2 CLI driver.** The vendored `clidriver` is proprietary
  and gitignored; it remains a user-supplied dependency in every consumption mode.

## 2. Decisions (locked)

| Topic | Decision |
|---|---|
| Distribution target | Source consumption only (no registry submission) |
| Version | Bump `0.1.0` → `1.0.0`, single-sourced from the CMake project version |
| Consumption modes | `find_package` (existing) **and** `FetchContent`/`add_subdirectory` (new) |
| Docs | MkDocs Material (guide) **+** Doxygen (API reference), one GitHub Pages site |
| Doxygen integration | Standalone Doxygen HTML under `/api/`, linked from the MkDocs nav |
| CMake floor | Keep `3.20` via a manual top-level fallback (do **not** raise to 3.21) |
| CI platforms | Linux (existing) **+** macOS (new build-test job) |
| Release cut | Land release-ready on a branch with prepared notes; tagging is a manual final step |
| Driver dependency | IBM `clidriver` stays user-supplied; consumers set `DB2_CLIDRIVER_ROOT` |

## 3. Workstream A — Version & release hygiene

### A.1 Single-source the version

Today `0.1.0` is duplicated in three places that can drift:

- `CMakeLists.txt` — `project(Halcyon VERSION 0.1.0 ...)`
- `include/halcyon/version.hpp` — `version_major/minor/patch` literals
- `tests/smoke/main.cpp` — `return v == "0.1.0" ? 0 : 1;`

**Decision:** the CMake `project(... VERSION ...)` is the single source of truth.
Generate the version header from a template via `configure_file`:

- Rename `include/halcyon/version.hpp` → `include/halcyon/version.hpp.in`, with
  `@PROJECT_VERSION_MAJOR@` / `@PROJECT_VERSION_MINOR@` / `@PROJECT_VERSION_PATCH@`
  placeholders (and a full `@PROJECT_VERSION@` string).
- `configure_file` generates the concrete header into the build tree
  (`${CMAKE_CURRENT_BINARY_DIR}/generated/include/halcyon/version.hpp`); add that
  directory to `halcyon`'s `PUBLIC` include path with a `$<BUILD_INTERFACE:...>`
  generator expression, and install the generated header to the same
  `include/halcyon/` location as the others.
- The smoke test asserts against the version reported by the linked library
  rather than a hardcoded literal. `run_smoke_test.cmake` already receives values
  from the parent build; it passes the expected `PROJECT_VERSION` to the smoke
  consumer as a compile definition, and the consumer compares the linked library's
  `halcyon::version()` against it. The test therefore never needs hand-editing on
  a bump.

**Verification:** `find`/`grep` confirms no hand-maintained version literal remains
outside `CMakeLists.txt`; configuring the project regenerates the header; the
existing version unit test still passes against the generated value.

### A.2 Bump to `1.0.0`

Change the single source (`project(Halcyon VERSION 1.0.0 ...)`); everything else
(header, smoke test, package config version file) follows automatically.

### A.3 `CHANGELOG.md`

Add a top-level `CHANGELOG.md` in [Keep a Changelog](https://keepachangelog.com/)
format with an `[Unreleased]` section and a seeded `[1.0.0]` entry summarizing the
shipped v1 capability set (drawn from the existing plans/commit history): core
data path, pooling/concurrency, statement cache, array binding, observability,
streaming reads, async.

### A.4 Compatibility policy

A short "Versioning & compatibility" section (in `README.md`, referenced from
`CHANGELOG.md`) stating:

- Halcyon follows SemVer for its **public C++ API** (`include/halcyon/`, excluding
  `include/halcyon/detail/`).
- `detail::` is not part of the stable surface.
- The IBM `clidriver` is a user-supplied dependency; Halcyon does not redistribute
  it.
- No binary-ABI guarantee — consume from source.

### A.5 Release procedure

Add `docs/RELEASING.md`: a checklist that runs the full verification matrix
(unit + smoke, stress/TSan, live-Db2 integration, both consumption tests, docs
build), updates `CHANGELOG.md`, and describes cutting the tag + GitHub Release.
Prepare the `1.0.0` release notes. **Cutting the actual git tag / GitHub Release is
left as the maintainer's final manual step** — this project lands everything
release-ready on a branch.

## 4. Workstream B — Consumption modes

### B.1 `FetchContent`/`add_subdirectory` correctness

**Problem:** `HALCYON_BUILD_TESTS` defaults `ON`. A downstream project that pulls
Halcyon via `FetchContent_MakeAvailable` / `add_subdirectory` would build
Halcyon's test tree and fetch GoogleTest into the consumer's build — wrong and
surprising.

**Fix:** detect whether Halcyon is the top-level project and default the
developer-only options off when it is not:

```cmake
# PROJECT_IS_TOP_LEVEL exists in CMake >= 3.21; define a fallback to keep the
# 3.20 floor (do not raise cmake_minimum_required).
if(NOT DEFINED PROJECT_IS_TOP_LEVEL)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(PROJECT_IS_TOP_LEVEL TRUE)
    else()
        set(PROJECT_IS_TOP_LEVEL FALSE)
    endif()
endif()

option(HALCYON_BUILD_TESTS    "Build tests"    ${PROJECT_IS_TOP_LEVEL})
option(HALCYON_BUILD_EXAMPLES "Build examples" OFF)
# HALCYON_BUILD_SMOKE_TEST already nested under HALCYON_BUILD_TESTS.
```

So a consumer gets only the `halcyon` library target and its usage requirements;
a top-level developer build is unchanged (tests on by default). Examples/stress/
integration stay off by default in both modes.

### B.2 Subdir-mode target correctness

Confirm that when added as a subdirectory:

- `halcyon::halcyon` (the `ALIAS`) is usable by the parent project (it is created
  unconditionally in `CMakeLists.txt`, so this should already hold — verify).
- `DB2::CLI` usage requirements propagate so the consumer links `db2` and inherits
  the driver include/RPATH, exactly as in install mode.
- The consumer still controls `DB2_CLIDRIVER_ROOT` (forwarded to `FindDB2CLI`).

### B.3 `FetchContent` consumption test

Add a CTest case parallel to the existing install smoke test
(`run_smoke_test.cmake` / `halcyon_install_smoke`): a tiny standalone consumer
project that obtains Halcyon via `add_subdirectory` (simulating the
`FetchContent_MakeAvailable` result, pointed at the source tree) and:

- compiles against `<halcyon/halcyon.hpp>`,
- links `halcyon::halcyon`,
- constructs (does not connect) the real CLI driver to prove `DB2::CLI`
  propagation,
- reports the version.

This test is gated like the install smoke test (top-level dev builds only). It
proves the subdir options-defaulting and target propagation without a live DB.

### B.4 Consumer documentation

Add to `README.md` (and the docs site) side-by-side **`find_package`** and
**`FetchContent`** snippets, each explicitly showing the `DB2_CLIDRIVER_ROOT`
requirement:

```cmake
# find_package (installed Halcyon)
find_package(Halcyon 1.0 REQUIRED)
target_link_libraries(my_app PRIVATE halcyon::halcyon)

# FetchContent (from source)
include(FetchContent)
FetchContent_Declare(Halcyon
    GIT_REPOSITORY https://github.com/<owner>/Halcyon.git
    GIT_TAG        v1.0.0)
FetchContent_MakeAvailable(Halcyon)
target_link_libraries(my_app PRIVATE halcyon::halcyon)
# Both modes require the consumer to provide the IBM Db2 CLI driver, e.g.
#   cmake -S . -B build -DDB2_CLIDRIVER_ROOT=/path/to/clidriver
```

## 5. Workstream C — Documentation site

### C.1 Narrative guide — MkDocs Material

- Add `mkdocs.yml` at the repo root with the Material theme, configured nav over
  the existing `docs/guide/*.md` (Getting Started → Error Handling → Querying →
  Parameters & Types → Connection Pool → Transactions → Batch Operations → Async
  → Observability → Advanced), search, and code highlighting.
- Reuse the existing markdown as-is; only minimal edits (front-matter/nav titles,
  relative-link fixes) as needed. `docs/guide/` remains the source.
- Build dependencies pinned in `docs/requirements.txt`
  (`mkdocs-material`, plus any needed extensions).

### C.2 API reference — Doxygen

- Add a `Doxyfile` (or `docs/Doxyfile`) configured to scan `include/halcyon/`
  (the public surface), excluding `include/halcyon/detail/` from the user-facing
  reference. Enable `JAVADOC_AUTOBRIEF`/`EXTRACT_ALL` so existing `//` comments are
  surfaced, output HTML.
- **Light doc-comment pass:** annotate the umbrella header and the principal public
  types (`Database`, `Connection`, `Transaction`/`ScopedTransaction`, `ResultSet`/
  `Row`, `Result<T>`, `Error`, `PoolConfig`, `batchOf`/`Batch`, async entry points)
  with brief Doxygen briefs/`@param`/`@return` where they materially improve the
  reference. This is a targeted pass, **not** an exhaustive rewrite of every
  symbol.

### C.3 Integration & publishing

- Build order: run `mkdocs build` first (producing `site/`), then run Doxygen with
  its output directory set to `site/api/`. The Doxygen HTML is thus published
  verbatim under `/api/`, and the MkDocs nav links to `api/index.html`. No
  MkDocs↔Doxygen theme-bridge plugin (robustness over visual unification). The
  Doxygen output is never committed — it is a build artifact under `site/`, which
  is gitignored.
- A single script (`docs/build.sh`, documented in `docs/README.md` and
  `docs/RELEASING.md`) runs both steps in order so local and CI builds are
  identical.
- Publishing is via the CI docs job (Workstream D) to **GitHub Pages**.

## 6. Workstream D — CI broadening

Keep every existing job unchanged (lint, gcc/clang `build-test` matrix with
warnings-as-errors, `stress-tsan`, live-Db2 `integration` + install/`find_package`
orders sample). Add:

### D.1 `fetchcontent` consumption job
Runs the new `FetchContent` consumption test (B.3) on Linux — configure + build +
`ctest -R` the FetchContent case (no live DB needed; uses the `setup-db2-clidriver`
action for the driver, same as other no-DB jobs).

### D.2 `docs` job
- On **PRs and `main`:** install `docs/requirements.txt` + Doxygen, build the full
  site (`doxygen` then `mkdocs build --strict`) to catch broken nav/links.
- On **`main` (and tags):** deploy the built site to **GitHub Pages**
  (`actions/deploy-pages`), with the appropriate `pages: write` / `id-token: write`
  permissions and a `github-pages` environment.

### D.3 macOS `build-test` job
A `macos-14` (Apple-silicon) job mirroring the Linux `build-test`: set up the
driver, configure with `-DHALCYON_BUILD_TESTS=ON -DHALCYON_WARNINGS_AS_ERRORS=ON`,
build, and run `ctest -LE integration` (unit + smoke; no live DB). This gives the
supported-but-untested macOS platform real CI coverage. The macOS driver
quarantine caveat from `AGENTS.md` applies; the `setup-db2-clidriver` action (or a
macOS branch of it) must clear quarantine on the fetched driver. If a hosted macOS
runner cannot run the GSKit-dependent driver load even for the no-connect smoke
test, the job degrades to build-only + unit tests that do not instantiate the real
driver, documented inline.

## 7. Testing strategy

| Workstream | Verification |
|---|---|
| A — version single-source | Reconfigure regenerates `version.hpp`; no stray literals; version unit test green; smoke test passes without hand-editing |
| A — `1.0.0` bump | `halcyon::version()` reports `1.0.0`; `HalcyonConfigVersion.cmake` reports `1.0.0` |
| B — subdir defaults | New `FetchContent` consumption test: consumer build does **not** build Halcyon tests or fetch GoogleTest; links and runs |
| B — find_package | Existing `halcyon_install_smoke` still passes unchanged |
| C — docs | `doxygen` + `mkdocs build --strict` succeed locally and in CI with no warnings/broken links |
| D — CI | All existing jobs green; new fetchcontent, docs, and macOS jobs green |

Final gate: full local matrix per `AGENTS.md` (unit + smoke, TSan stress, live-Db2
integration, both consumption tests, docs build) all green before the branch is
considered release-ready.

## 8. Risks & subtleties

- **Proprietary driver, every mode.** `clidriver` cannot be bundled; all
  consumption docs and tests must make `DB2_CLIDRIVER_ROOT` explicit. Consumption
  tests construct-but-don't-connect to avoid needing a live DB.
- **RPATH carries the builder's driver path.** `INSTALL_RPATH`/`BUILD_RPATH` bake
  `DB2CLI_LIBRARY_DIR`; for source consumption this is fine (the consumer builds
  Halcyon with their own `DB2_CLIDRIVER_ROOT`), but the docs must state that the
  driver lib dir must remain present at runtime.
- **CMake floor.** We keep `3.20` via the manual `PROJECT_IS_TOP_LEVEL` fallback
  rather than raising the minimum and pushing that requirement onto consumers.
- **Doxygen coverage is intentionally light.** The API reference surfaces existing
  comments plus a targeted annotation pass; it is not a promise of exhaustive
  per-symbol documentation.
- **macOS driver load in CI.** Hosted macOS runners may not be able to `dlopen`
  the unsigned GSKit libs; the macOS job has a documented build-only/unit fallback
  so it never becomes a flaky blocker.
- **Version-header generation must not break IDE/tooling** that currently reads the
  in-tree `version.hpp`. Removing the tracked header in favor of a generated one
  requires updating any include paths and `.gitignore` for the generated artifact;
  the build-tree generated dir is added to the public include interface so normal
  builds resolve it.

## 9. Out-of-scope follow-ups (future versions)

Recorded so they are not lost, but **not** part of this project: vcpkg port + Conan
recipe and registry submission; the deferred feature directions (savepoints/nested
transactions/isolation levels, true LOB streaming, the C++20 coroutine layer);
Windows/AIX platforms; versioned documentation.
