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
ctest --test-dir build --output-on-failure          # unit tests (mock the seam, no DB)
```

Key CMake options: `HALCYON_WITH_PROMETHEUS`, `HALCYON_WITH_OTEL`,
`HALCYON_BUILD_TESTS`, `HALCYON_BUILD_INTEGRATION_TESTS`, `HALCYON_BUILD_EXAMPLES`,
`HALCYON_WARNINGS_AS_ERRORS`.
The vendored driver is found at `third_party/clidriver`; override with
`-DDB2_CLIDRIVER_ROOT=...`. The driver lib dir is added to the RPATH, so examples
and tests run without setting `LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH`. On macOS,
`FindDB2CLI.cmake` rewrites `libdb2.dylib`'s bare install name to
`@rpath/libdb2.dylib` at configure time (idempotent) so the RPATH actually
resolves it — dyld does not search RPATH for bare dependent names.

### Concurrency stress & performance suite (opt-in)

Build with `-DHALCYON_BUILD_STRESS_TESTS=ON`. The correctness suite is a CTest
target labeled `stress`; build it under a sanitizer and run it to verify
race/deadlock freedom:

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON \
      -DHALCYON_BUILD_STRESS_TESTS=ON -DHALCYON_SANITIZER=thread
cmake --build build -j
ctest --test-dir build -L stress --output-on-failure
./build/tests/stress/halcyon_stress --scenario=all --threads=1,2,4,8,16   # perf report
```

See `tests/stress/README.md` for details.

### Integration tests against live Db2 (Docker)

The integration suite (`tests/integration/`, CTest label `integration`) is
opt-in and only *runs* when `HALCYON_TEST_DSN` is set; otherwise each test
reports as skipped. The full verified procedure:

```bash
# 1. Configure + build with the integration target enabled.
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON -DHALCYON_BUILD_INTEGRATION_TESTS=ON
cmake --build build -j

# 2. Start Db2 and wait until the container reports healthy (first boot creates
#    the SAMPLE database; ~2 min native, longer under amd64 emulation on arm64).
docker compose -f docker/docker-compose.yml up -d
docker compose -f docker/docker-compose.yml ps   # wait for STATUS = healthy

# 3. Point the tests at the container and run them.
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
ctest --test-dir build -L integration --output-on-failure

# 4. Tear down when done.
docker compose -f docker/docker-compose.yml down
```

The compose file (`docker/docker-compose.yml`) uses `icr.io/db2_community/db2`
with `DBNAME=SAMPLE` and `DB2INST1_PASSWORD=halcyon` on port `50000`, matching
the DSN above.

#### Apple `container` as a Docker alternative (macOS)

On macOS the Db2 service can also be brought up with Apple's native
[`container`](https://github.com/apple/container) runtime instead of Docker —
see `docker/README.md` for the full command. The key differences:

- Start the service once with `container system start`, and install a guest
  kernel once with `container system kernel set --recommended` (no default kernel
  ships for arm64 hosts).
- There is no Compose file and no `--privileged` flag (each container is its own
  VM). Start Db2 with a single `container run -d --platform linux/amd64 -p 50000:50000 ...`.
- `-p 50000:50000` publishes to `localhost`, so the same `HALCYON_TEST_DSN`
  works; each container also has its own routable IP shown by `container ls`.
- **Caveat (verified):** the Db2 image is amd64-only. Under Apple `container`'s
  amd64 emulation on Apple silicon the engine fails to start (`db2start` →
  `SQL1042C`, `db2diag.log` shows `db2sysc exited prematurely`), so `SAMPLE`
  never becomes connectable. Use **Docker** for integration tests on Apple
  silicon; Apple `container` works end-to-end only on amd64 hosts.

**macOS one-time step (Gatekeeper / GSKit).** The vendored driver `dlopen`s its
IBM GSKit security libraries (`third_party/clidriver/lib/icc/libgsk8*.dylib`)
during `connect`. Those binaries are unsigned and arrive with the
`com.apple.quarantine` attribute, so Gatekeeper blocks the load and `connect`
fails with `SQL1042C ... SQLSTATE=58004` (and Finder/XProtect flags
`libgsk8sys.dylib` as malware). If you trust the source of your vendored driver,
clear quarantine once after fetching it (the files are read-only, so make them
writable first):

```bash
chmod -R u+w third_party/clidriver
xattr -r -d com.apple.quarantine third_party/clidriver
```

This disables a macOS security mitigation on those binaries — only do it for a
driver whose provenance you trust.

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
- **Verification before finish implementation — ALWAYS run ALL tests, INCLUDING integration tests.** At any
  verification stage (before claiming work complete, before merging, before a
  PR), run the full suite, not just the unit tests. The integration suite is not
  optional here: bring up live Db2 and run it (see "Integration tests against
  live Db2" above — `HALCYON_BUILD_INTEGRATION_TESTS=ON`, start the container,
  set `HALCYON_TEST_DSN`, `ctest -L integration`). Never treat a `Skipped`
  integration result as verification — that means the tests did not run. Tear the
  container down when done.
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
