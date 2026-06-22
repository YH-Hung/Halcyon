# Halcyon Orders Sample

A self-contained sample showing how to consume Halcyon from a downstream
project and perform **query / insert / update** against a local IBM Db2, in two
parallel styles that implement identical logic:

- `orders_oo` — object-oriented / throwing API (`openOrThrow`, `queryAsOrThrow`, …)
- `orders_functional` — functional / `Result<T>` API (`halcyon::query_as`, …)

Both run the same seven steps over a mini order system (`customers` 1:N
`orders`): connect → query customers (typed struct mapping) → query orders by a
named parameter → insert an order → batch-insert orders → update orders inside a
transaction → re-query. Shared row structs and SQL live in
`src/orders_model.hpp`.

## Prerequisites

- Docker (the repo's `docker/docker-compose.yml` provides Db2 `SAMPLE`).
- A Halcyon install prefix (see step 3).
- The vendored CLI driver at `third_party/clidriver` (see the repo `AGENTS.md`).
  On macOS, clear Gatekeeper quarantine on the driver once before building:

  ```bash
  chmod -R u+w third_party/clidriver
  xattr -r -d com.apple.quarantine third_party/clidriver
  ```

  The `chmod` must come first: some bundled GSKit libraries under `lib/icc/`
  (e.g. `libgsk8sys.dylib`) ship read-only, and `xattr -d` cannot strip the
  attribute from a non-writable file. If any `icc/` lib stays quarantined the
  sample builds fine but fails at connect time with `SQL1042C`. Verify with
  `find third_party/clidriver/lib/icc -name '*.dylib' -exec xattr {} \;`
  (no output = clean).

## 1. Start Db2

```bash
docker compose -f ../../docker/docker-compose.yml up -d
docker compose -f ../../docker/docker-compose.yml ps   # wait for STATUS = healthy
```

The first boot creates `SAMPLE` (~2 min native; longer under amd64 emulation).

## 2. Load the schema and seed data

```bash
./load_sql.sh
```

This copies `sql/schema.sql` and `sql/seed.sql` into the container and applies
them with `db2 -tvf`. On a fresh database the `DROP` lines print `SQL0204N`
(harmless). The raw equivalent, if you prefer to run it by hand:

```bash
docker compose -f ../../docker/docker-compose.yml cp sql/schema.sql db2:/tmp/schema.sql
docker compose -f ../../docker/docker-compose.yml exec db2 \
  su - db2inst1 -c "{ echo 'CONNECT TO SAMPLE;'; cat /tmp/schema.sql; } > /tmp/run.sql && db2 -tvf /tmp/run.sql"
# …repeat for seed.sql
```

Apple `container` users: there is no Compose; use `container cp` and
`container exec halcyon-db2 bash -lc 'su - db2inst1 -c ". ~/sqllib/db2profile; { echo CONNECT TO SAMPLE\;; cat /tmp/schema.sql; } > /tmp/run.sql && db2 -tvf /tmp/run.sql"'`
(see `docker/README.md`). Note: Db2's image is amd64-only and does not start
under Apple `container` on Apple silicon — use Docker there.

## 3. Build the sample

First, build and install Halcyon to a prefix from the Halcyon repo root:

```bash
# From the Halcyon repo root: build and install Halcyon to a prefix.
cmake -S . -B build
cmake --build build -j
cmake --install build --prefix build/install
```

Then configure and build the sample:

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=<halcyon-repo>/build/install \
  -DDB2_CLIDRIVER_ROOT=<halcyon-repo>/third_party/clidriver
cmake --build build -j
```

`find_package(Halcyon)` needs `CMAKE_PREFIX_PATH` to point at a Halcyon install
prefix. `DB2_CLIDRIVER_ROOT` must point at Halcyon's vendored driver, because
the driver's default search root is relative to *this* project. Halcyon adds
the driver lib dir to the RPATH, so no `DYLD_LIBRARY_PATH` /
`LD_LIBRARY_PATH` is needed at run time.

## 4. Run

```bash
export HALCYON_TEST_DSN="DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;"
./build/orders_oo
./build/orders_functional
```

If `HALCYON_TEST_DSN` is unset, both binaries fall back to the same localhost
DSN shown above.

### Expected output (identical for both binaries)

```
== Connected ==

== Customers ==
  #1  Ada Lovelace  <ada@example.com>  joined 2026-01-15
  #2  Linus Torvalds  <(no email)>  joined 2026-02-03
  #3  Grace Hopper  <grace@example.com>  joined 2026-03-21

== Orders for customer #1 ==
  ORD-1001  cust#1  PAID  $199.99  2026-04-01 10:00:00.000000
  ORD-1002  cust#1  NEW  $49.50  2026-04-02 11:30:00.000000

== Inserted 1 order ==
== Batch inserted 2 orders ==
== Transaction updated 2 orders ==

== ORD-2001 after update ==
  ORD-2001  cust#1  SHIPPED  $129.99  2026-06-22 09:30:00.000000

== Done ==
```

Dates and timestamps print in Db2's text form (`DATE` as `YYYY-MM-DD`,
`TIMESTAMP` as `YYYY-MM-DD HH:MM:SS.ffffff`). The sample deletes its demo rows
(`ORD-2001`..`ORD-2003`) on startup, so it is safe to run repeatedly.

## 5. Tear down

```bash
docker compose -f ../../docker/docker-compose.yml down
```
