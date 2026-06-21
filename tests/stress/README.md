# Halcyon concurrency stress & performance suite

Two front-ends over one shared core (`support/`):

- **`halcyon_stress_tests`** — GoogleTest correctness suite (CTest label `stress`).
  Runs against the in-process `ConcurrentFakeDriver` (no DB). Build it under a
  sanitizer to prove race/deadlock freedom.
- **`halcyon_stress`** — standalone perf harness. Reports throughput / latency /
  scaling for each scenario against the fake or a live Db2 (`HALCYON_TEST_DSN`).

## Build

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON \
      -DHALCYON_BUILD_STRESS_TESTS=ON -DHALCYON_SANITIZER=thread
cmake --build build -j
```

`HALCYON_SANITIZER` accepts `thread`, `address`, `undefined`, or empty (none).

## Correctness (sanitizer-clean)

```bash
ctest --test-dir build -L stress --output-on-failure
```

A clean run under `-DHALCYON_SANITIZER=thread` is the evidence of race freedom.

## Performance

```bash
# fake backend (pure library overhead), scaling sweep
./build/tests/stress/halcyon_stress --scenario=all --threads=1,2,4,8,16

# add simulated per-call I/O so pooling has something to overlap (otherwise a
# zero-latency fake just serialises on its own mutex and the scaling gate WARNs)
./build/tests/stress/halcyon_stress --scenario=pool --threads=2,8,16 --latency=200

# live Db2 (report-only)
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
./build/tests/stress/halcyon_stress --backend=live --scenario=pool --threads=4,8,16

# CSV for plotting; --strict makes soft gates set the exit code
./build/tests/stress/halcyon_stress --format=csv --strict > sweep.csv
```

`--scenario` accepts a comma list (e.g. `--scenario=pool,executor,cache,txn`) or
`all`, just like `--threads`. `--duration` and `--warmup` are integer
**milliseconds** (the warmup window runs untimed and is discarded before the timed
window, spec §5.4); `--latency` is the fake's per-call latency in **microseconds**.

The three soft gates (spec §7.2) are reported as `PASS`/`WARN` and only affect the
exit code under `--strict` (fake backend only — live numbers are report-only):

- **scaling** — peak throughput ≥ `--scaling-mult` × base (default 1.5)
- **latency** — worst p99/p50 ≤ `--latency-mult` (default 50)
- **no-starvation** — worst tolerated error-rate ≤ `--starvation-max` (default 0.25)
