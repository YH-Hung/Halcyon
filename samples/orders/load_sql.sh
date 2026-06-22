#!/usr/bin/env bash
# Loads the orders sample schema + seed into the Db2 SAMPLE database running in
# the docker-compose container (docker/docker-compose.yml). Run from anywhere;
# paths resolve relative to this script.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
COMPOSE=(docker compose -f "${REPO_ROOT}/docker/docker-compose.yml")
SERVICE="db2"

for f in schema.sql seed.sql; do
    echo "==> loading ${f}"
    "${COMPOSE[@]}" cp "${SCRIPT_DIR}/sql/${f}" "${SERVICE}:/tmp/${f}"
    # db2 -tvf exits 4 when any statement fails (e.g. DROP on a non-existent table);
    # that is expected on a fresh database and is harmless, so we allow non-zero exits.
    "${COMPOSE[@]}" exec -T "${SERVICE}" \
        su - db2inst1 -c "{ echo 'CONNECT TO SAMPLE;'; cat /tmp/${f}; } > /tmp/_run.sql && db2 -tvf /tmp/_run.sql" || true
done

echo "==> done"
