#!/usr/bin/env bash
set -euo pipefail

compose_file="docker/docker-compose.yml"
service="db2"
if (( $# > 0 )); then
  compose_file="$1"
  shift
fi
if (( $# > 0 )); then
  service="$1"
  shift
fi

timeout_seconds="${DB2_READY_TIMEOUT_SECONDS:-600}"
poll_seconds="${DB2_READY_POLL_SECONDS:-5}"
stable_probes="${DB2_READY_STABLE_PROBES:-3}"

deadline=$((SECONDS + timeout_seconds))

log() {
  printf '[db2-ready] %s\n' "$*"
}

container_id() {
  docker compose -f "$compose_file" ps -q "$service"
}

require_time_left() {
  if (( SECONDS >= deadline )); then
    log "timed out after ${timeout_seconds}s waiting for Db2 readiness"
    docker compose -f "$compose_file" ps || true
    docker compose -f "$compose_file" logs --no-color "$service" || true
    exit 1
  fi
}

id="$(container_id)"
if [[ -z "$id" ]]; then
  log "service '$service' is not running in $compose_file"
  docker compose -f "$compose_file" ps || true
  exit 1
fi

log "waiting for Db2 entrypoint setup to complete"
until [[ "$(docker logs "$id" 2>&1 || true)" == *"Setup has completed"* ]]; do
  require_time_left
  if [[ "$(docker inspect -f '{{.State.Running}}' "$id")" != "true" ]]; then
    log "container stopped before setup completed"
    docker compose -f "$compose_file" logs --no-color "$service" || true
    exit 1
  fi
  sleep "$poll_seconds"
done

log "entrypoint setup completed; probing host-side connectivity"
if (( $# > 0 )); then
  probe_cmd=("$@")
else
  probe_cmd=(
    docker compose -f "$compose_file" exec -T "$service"
    su - db2inst1 -c 'db2 connect to SAMPLE >/dev/null && db2 terminate >/dev/null'
  )
fi

consecutive_successes=0
while (( consecutive_successes < stable_probes )); do
  require_time_left
  if "${probe_cmd[@]}"; then
    consecutive_successes=$((consecutive_successes + 1))
    log "probe ${consecutive_successes}/${stable_probes} passed"
  else
    consecutive_successes=0
    log "probe failed; waiting for Db2 to settle"
  fi
  if (( consecutive_successes < stable_probes )); then
    sleep "$poll_seconds"
  fi
done

log "Db2 is ready"
