#!/bin/bash
# DZFoot GF Docker Worker launcher — reads .env and starts the worker container.
# Called by systemd gf-worker-docker.service.

set -euo pipefail

ENV_FILE="/opt/dzfoot-gf-server/.env"

if [[ ! -f "$ENV_FILE" ]]; then
    echo "FATAL: $ENV_FILE not found." >&2
    exit 1
fi

# Source .env (export all KEY=VALUE lines)
set -a
source "$ENV_FILE"
set +a

# Validate required vars
if [[ -z "${REDIS_URL:-}" ]]; then
    echo "FATAL: REDIS_URL is not set in $ENV_FILE" >&2
    exit 1
fi

IMAGE="djdreamer79/dzfoot-gf-server:latest"
NETWORK="${GF_NETWORK:-dzfoot-network}"
LOGS_PATH="${GF_LOGS_HOST_PATH:-/var/log/dzfoot/gf}"

# Pull latest image before starting
docker pull "$IMAGE" >/dev/null 2>&1 || true

exec docker run --rm --name gf-worker \
  -v /var/run/docker.sock:/var/run/docker.sock \
  -v "$LOGS_PATH:/app/gf_logs:rw" \
  -e REDIS_URL="$REDIS_URL" \
  -e STATS_URL="${STATS_URL:-}" \
  -e LIVEKIT_URL="${LIVEKIT_URL:-}" \
  -e GF_IMAGE="$IMAGE" \
  -e GF_NETWORK="$NETWORK" \
  -e GF_LOGS_HOST_PATH="$LOGS_PATH" \
  "$IMAGE" \
  python3 /app/gf_worker_docker.py
