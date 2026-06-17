# AGENTS.md

Agent guidance for the **Halcyon** repository. Keep this file current as the
project evolves.

## What this project is

Halcyon is a modern **C++17** client library for **IBM Db2**, built on the IBM
Db2 CLI (Call Level Interface, `sqlcli1.h`). It provides a high-level, ergonomic,
type-safe API in both object-oriented/fluent and functional styles, with thread
safety, connection pooling, transparent reconnect + safe auto-retry on transient
failures, `std::future` async (coroutine-ready), and optional observability via
`prometheus-cpp` (metrics) and `opentelemetry-cpp` (tracing).

- **Design spec:** `docs/superpowers/specs/2026-06-17-halcyon-db2-cpp-design.md`
- **Implementation plans:** `docs/superpowers/plans/` (start with the foundation plan)

Read the spec before making non-trivial changes. It is the source of truth for
architecture and decisions.

## Architecture (layered — "thin CLI seam")

Dependency direction (`A ► B` = A depends on B):

```
facade ─► pool ─► core ─► detail::cli ─► sqlcli1.h
   │                          ▲
   └──► observability ────────┘   (interfaces only; adapters optional)
```

**Invariant:** only code under `src/detail/cli/` and `cmake/FindDB2CLI.cmake` may
reference IBM CLI specifics (`sqlcli1.h`, `SQL*` types, the `db2` library).
Everything above the seam (`ICliDriver`) must stay portable and mockable. Do not
leak `sqlcli1.h` types into public headers.

## Repository layout

- `include/halcyon/` — public headers (umbrella: `halcyon.hpp`).
- `include/halcyon/detail/cli/` — the seam (`ICliDriver`, opaque handles).
- `src/{detail/cli,core,pool,facade,observability}/` — implementation by layer.
- `tests/unit/` — unit tests; mock the seam via `MockCliDriver` (no live DB).
- `tests/integration/` — Dockerized Db2 tests (CTest label `integration`, opt-in).
- `cmake/` — `FindDB2CLI.cmake` and package config.
- `third_party/clidriver/` — vendored IBM Db2 CLI driver (gitignored).
- `examples/`, `docker/` — usage samples and the Db2 compose for integration tests.

## Build & test

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure          # unit tests
ctest --test-dir build -L integration               # integration (needs Docker Db2)
```

Key CMake options: `HALCYON_WITH_PROMETHEUS`, `HALCYON_WITH_OTEL`,
`HALCYON_BUILD_TESTS`, `HALCYON_BUILD_EXAMPLES`, `HALCYON_WARNINGS_AS_ERRORS`.
The vendored driver is found at `third_party/clidriver`; override with
`-DDB2_CLIDRIVER_ROOT=...`. The driver lib dir is added to the RPATH, so examples
and tests run without setting `LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH`.

## Conventions

- **Standard:** C++17 only. No C++20 features (coroutines are a *future* layer;
  keep extension points, don't add awaitables yet).
- **Errors:** dual model. Recoverable paths return `Result<T>`; throwing overloads
  unwrap via `Result::value()` / `throw_error`. Never let `sqlcli1.h` error codes
  escape the seam un-translated — map SQLSTATE → `ErrorCode` in `detail::cli`.
- **Ownership:** RAII everywhere; value semantics by default. A single
  `Connection`/`Transaction`/`ResultSet` is single-owner (not shared across
  threads); `Database`/`ConnectionPool` are fully thread-safe.
- **Naming:** `lower_snake_case` for files; types `PascalCase`; functions/methods
  `camelCase` for the public API, matching the spec's examples.
- **Headers:** `#pragma once`; include what you use; keep public headers free of
  heavy/internal includes.
- **Style:** `-Wall -Wextra -Wpedantic` clean. Run `clang-format` and
  `clang-tidy` before committing. Prefer small, focused files.

## Workflow expectations

- **TDD:** follow the plans — write the failing test, see it fail, implement the
  minimum, see it pass, commit. Keep commits small and focused.
- **Conventional commits:** `feat:`, `fix:`, `build:`, `docs:`, `chore:`, `test:`.
- **Do not** commit anything under `third_party/` or build artifacts (gitignored).
- **Do not** add hard dependencies on `prometheus-cpp`/`opentelemetry-cpp` in core;
  they are optional, behind interfaces, gated by CMake options.
- Only commit when the user asks. Don't push without being asked.

## Where to start a task

1. Read the spec section relevant to your change.
2. Find or write the matching plan task under `docs/superpowers/plans/`.
3. Implement against the seam with unit tests first; add integration coverage when
   touching real driver behavior.
