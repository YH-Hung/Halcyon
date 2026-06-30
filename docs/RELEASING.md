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
