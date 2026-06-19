# Db2 for integration tests

The Halcyon integration suite (`tests/integration/`, CTest label `integration`)
needs a live IBM Db2 with the `SAMPLE` database. This directory provides the
container setup. The suite only *runs* when `HALCYON_TEST_DSN` is set; otherwise
every test reports as skipped.

Build the integration target first:

```bash
cmake -S . -B build -DHALCYON_BUILD_TESTS=ON -DHALCYON_BUILD_INTEGRATION_TESTS=ON
cmake --build build -j
```

The image (`icr.io/db2_community/db2:11.5.9.0`) is published for **linux/amd64
only**, so on Apple silicon / arm64 hosts it runs under emulation (see the
caveat at the end).

## Docker (default)

```bash
# 1. Start Db2 and wait until the container reports healthy. The first boot
#    creates the SAMPLE database (~2 min native; longer under amd64 emulation).
docker compose -f docker/docker-compose.yml up -d
docker compose -f docker/docker-compose.yml ps   # wait for STATUS = healthy

# 2. Point the tests at the container and run them.
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
ctest --test-dir build -L integration --output-on-failure

# 3. Tear down when done.
docker compose -f docker/docker-compose.yml down
```

`docker-compose.yml` runs the image `privileged` with `DBNAME=SAMPLE`,
`DB2INST1_PASSWORD=halcyon`, and publishes port `50000`, matching the DSN above.

## Apple `container` (Docker alternative on macOS)

[Apple `container`](https://github.com/apple/container) is Apple's native
container runtime for macOS (each container runs in its own lightweight VM). It
can be used instead of Docker. There is no Compose file equivalent, so the Db2
service is started with a single `container run`.

```bash
# 1. Start the container system service (one-time per login session) and, the
#    first time only, install a Linux kernel for the guest VMs.
container system start
container system kernel set --recommended      # only needed once

# 2. Pull and run Db2. The image is amd64-only, so pass --platform explicitly.
#    There is no --privileged flag — each container already runs in its own VM.
container run -d --name halcyon-db2 --platform linux/amd64 \
  -e LICENSE=accept -e DB2INST1_PASSWORD=halcyon \
  -e DBNAME=SAMPLE -e PERSISTENT_HOME=false \
  -p 50000:50000 -m 4g \
  icr.io/db2_community/db2:11.5.9.0

# 3. Wait for the database to come up, then watch the boot log until it prints
#    "Setup has completed."
container logs -f halcyon-db2

# 4. Point the tests at the container and run them.
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
ctest --test-dir build -L integration --output-on-failure

# 5. Tear down when done.
container rm -f halcyon-db2
```

Notes / differences from Docker:

- **Service + kernel.** `container system start` must be running, and a guest
  kernel must be installed once with `container system kernel set --recommended`
  (Apple `container` ships no default kernel for arm64 hosts).
- **No `--privileged`.** Db2's Compose file uses `privileged: true`; Apple
  `container` has no such flag because every container is an isolated VM. Drop
  it entirely.
- **Networking.** `-p 50000:50000` publishes the port to `localhost`, so the DSN
  above works unchanged. Each container also gets its own routable IP
  (`container ls` shows it), so `HOSTNAME=<container-ip>` works too.
- **`container exec`.** To run `db2` inside the container, source the instance
  profile first, e.g.
  `container exec halcyon-db2 bash -lc 'su - db2inst1 -c ". ~/sqllib/db2profile; db2 connect to SAMPLE"'`.

### Caveat: amd64 emulation on Apple silicon

The Db2 community image is **amd64-only**. Docker Desktop runs it via its bundled
QEMU emulation, which successfully starts the Db2 engine (`db2sysc`) — just
slowly. Apple `container`'s amd64 emulation on Apple silicon currently does **not**
start the engine: `db2start` fails with `SQL1042C` and `db2diag.log` shows
`db2sysc exited prematurely` (an IPC `msgrcv` / shared-memory failure). The
instance is created and configured, but `SAMPLE` never becomes connectable.

Practical guidance:

- On **Apple silicon**, use **Docker** for the integration tests until an arm64
  Db2 image (or improved Apple `container` x86 emulation) is available. Apple
  `container` is still useful here for building/pulling the image and inspecting
  the instance.
- On **amd64 macOS/Linux hosts**, the image runs natively and Apple `container`
  works end-to-end for the integration suite.
