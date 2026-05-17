#!/bin/bash
# DZFoot GF Worker — Runs on dedicated VMs, polls Redis for match spawn requests
# Circuit complet: heartbeat, ready signal, crash detection, timeout, reconnection

set -e

REDIS_URL="${REDIS_URL:-redis://localhost:6379}"
GF_BINARY="${GF_BINARY:-/usr/local/bin/gf_server}"
STATS_URL="${STATS_URL:-http://stats:8000}"
LOG_DIR="${LOG_DIR:-/var/log/dzfoot}"
HEARTBEAT_INTERVAL="${HEARTBEAT_INTERVAL:-10}"
WORKER_ID="${HOSTNAME:-worker-$(hostname)}"

mkdir -p "$LOG_DIR"

echo "[GF Worker $WORKER_ID] Starting. Redis: $REDIS_URL"
echo "[GF Worker $WORKER_ID] Binary: $GF_BINARY"

# Check prerequisites
if ! command -v redis-cli &> /dev/null; then
    echo "ERROR: redis-cli not found. Install redis-tools."
    exit 1
fi

if [ ! -x "$GF_BINARY" ]; then
    echo "ERROR: GF binary not found at $GF_BINARY"
    exit 1
fi

# === Heartbeat loop: announce this worker is alive ===
heartbeat() {
    while true; do
        redis-cli -u "$REDIS_URL" HSET "gf.workers" "$WORKER_ID" "$(date +%s)" >/dev/null 2>&1 || true
        sleep "$HEARTBEAT_INTERVAL"
    done
}
heartbeat &
HEARTBEAT_PID=$!

# === Cleanup on exit ===
cleanup() {
    echo "[GF Worker $WORKER_ID] Cleaning up..."
    kill "$HEARTBEAT_PID" 2>/dev/null || true
    redis-cli -u "$REDIS_URL" HDEL "gf.workers" "$WORKER_ID" >/dev/null 2>&1 || true
    # Terminate all active GF processes managed by this worker
    for room in $(redis-cli -u "$REDIS_URL" HKEYS "gf.active" 2>/dev/null || true); do
        pid=$(redis-cli -u "$REDIS_URL" HGET "gf.active" "$room" 2>/dev/null || true)
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
            redis-cli -u "$REDIS_URL" HDEL "gf.active" "$room" >/dev/null 2>&1 || true
            redis-cli -u "$REDIS_URL" PUBLISH "gf.crashed" "$room" >/dev/null 2>&1 || true
        fi
    done
    exit 0
}
trap cleanup SIGINT SIGTERM

# === Poll loop ===
while true; do
    # Block until a spawn request arrives (10s timeout)
    RESULT=$(redis-cli -u "$REDIS_URL" BRPOP "gf.spawn" 10 2>/dev/null || true)
    
    if [ -z "$RESULT" ] || [ "$RESULT" = "nil" ]; then
        continue
    fi
    
    # Parse: BRPOP returns [queue_name, json_payload]
    PAYLOAD=$(echo "$RESULT" | tail -n1)
    
    ROOM_ID=$(echo "$PAYLOAD" | python3 -c "import sys,json; print(json.load(sys.stdin)['room_id'])" 2>/dev/null || echo "")
    TOKEN=$(echo "$PAYLOAD" | python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null || echo "")
    TEAM_A=$(echo "$PAYLOAD" | python3 -c "import sys,json; print(json.load(sys.stdin).get('team_a','default-a'))" 2>/dev/null || echo "default-a")
    TEAM_B=$(echo "$PAYLOAD" | python3 -c "import sys,json; print(json.load(sys.stdin).get('team_b','default-b'))" 2>/dev/null || echo "default-b")
    STADIUM=$(echo "$PAYLOAD" | python3 -c "import sys,json; print(json.load(sys.stdin).get('stadium_id','default-stadium'))" 2>/dev/null || echo "default-stadium")
    DURATION=$(echo "$PAYLOAD" | python3 -c "import sys,json; print(json.load(sys.stdin).get('duration',600))" 2>/dev/null || echo "600")
    
    if [ -z "$ROOM_ID" ] || [ -z "$TOKEN" ]; then
        echo "[GF Worker $WORKER_ID] Invalid payload, skipping"
        continue
    fi
    
    LOG_FILE="$LOG_DIR/gf_${ROOM_ID}_$(date +%s).log"
    
    # Spawn GF process in background
    nohup "$GF_BINARY" \
        --room-id="$ROOM_ID" \
        --team-a="$TEAM_A" \
        --team-b="$TEAM_B" \
        --stadium="$STADIUM" \
        --duration="$DURATION" \
        --livekit-url="${LIVEKIT_URL:-}" \
        --livekit-token="$TOKEN" \
        --stats-url="$STATS_URL" \
        > "$LOG_FILE" 2>&1 &
    
    PID=$!
    echo "[GF Worker $WORKER_ID] Spawned GF for room $ROOM_ID (PID $PID, log: $LOG_FILE)"
    
    # Register in Redis
    redis-cli -u "$REDIS_URL" HSET "gf.active" "$ROOM_ID" "$PID" >/dev/null 2>&1 || true
    redis-cli -u "$REDIS_URL" HSET "gf.worker_map" "$ROOM_ID" "$WORKER_ID" >/dev/null 2>&1 || true
    redis-cli -u "$REDIS_URL" PUBLISH "gf.ready" "$ROOM_ID" >/dev/null 2>&1 || true
    
    # Watchdog: monitor GF process, detect crash
    (
        sleep 5  # Give GF time to init LiveKit
        if ! kill -0 "$PID" 2>/dev/null; then
            echo "[GF Worker $WORKER_ID] GF crashed immediately for $ROOM_ID"
            redis-cli -u "$REDIS_URL" HDEL "gf.active" "$ROOM_ID" >/dev/null 2>&1 || true
            redis-cli -u "$REDIS_URL" HDEL "gf.worker_map" "$ROOM_ID" >/dev/null 2>&1 || true
            redis-cli -u "$REDIS_URL" PUBLISH "gf.crashed" "$ROOM_ID" >/dev/null 2>&1 || true
            exit
        fi
        
        # Monitor until process dies
        while kill -0 "$PID" 2>/dev/null; do
            sleep 5
        done
        
        # GF exited normally or crashed
        EXIT_CODE=$?
        echo "[GF Worker $WORKER_ID] GF exited for $ROOM_ID (code $EXIT_CODE)"
        redis-cli -u "$REDIS_URL" HDEL "gf.active" "$ROOM_ID" >/dev/null 2>&1 || true
        redis-cli -u "$REDIS_URL" HDEL "gf.worker_map" "$ROOM_ID" >/dev/null 2>&1 || true
        
        if [ "$EXIT_CODE" -ne 0 ]; then
            redis-cli -u "$REDIS_URL" PUBLISH "gf.crashed" "$ROOM_ID" >/dev/null 2>&1 || true
        else
            redis-cli -u "$REDIS_URL" PUBLISH "gf.finished" "$ROOM_ID" >/dev/null 2>&1 || true
        fi
    ) &
done
