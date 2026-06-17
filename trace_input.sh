#!/bin/bash
# =============================================================================
# trace_input.sh — Filter dedicated input pipeline logs
# Usage:
#   ./trace_input.sh <room_id>
#
# Shows the full input journey:
#   ANDROID_OUT (client) → BOT_IN (backend) → FORWARD (backend) → GF_IN (server)
#   and actions:          GF_ACTION / GF_APPLY (server)
# =============================================================================

ROOM_ID="${1:-}"

if [ -z "$ROOM_ID" ]; then
    echo "Usage: $0 <room_id>"
    echo ""
    echo "This script traces the input pipeline across all services."
    echo "Run it on the server where logs are collected."
    echo ""
    echo "It searches for:"
    echo "  ANDROID_OUT   — Android client sending input"
    echo "  BOT_IN        — Backend session receiving input from LiveKit"
    echo "  FORWARD       — Backend session relaying input to Redis"
    echo "  GF_IN         — GF Server receiving input from Redis"
    echo "  GF_APPLY      — GF Server applying inputs to game tick"
    echo "  GF_ACTION     — GF Server executing player actions"
    exit 1
fi

echo "=== Input Pipeline Trace for room: $ROOM_ID ==="
echo ""

# 1. Native GF logs (gf-worker-native.py)
GF_LOG="/var/log/dzfoot/gf/gf_${ROOM_ID}.log"
if [ -f "$GF_LOG" ]; then
    echo "--- GF Server Log ($GF_LOG) ---"
    grep -E "GF_IN|GF_APPLY|GF_ACTION" "$GF_LOG" | tail -n 30
    echo ""
else
    echo "--- GF Server Log ($GF_LOG) NOT FOUND ---"
    echo "  If running Docker mode, use:  docker logs <gf_container>"
    echo ""
fi

# 2. Backend session logs (try local files first, then Docker)
SESSION_LOG_PATTERNS=(
    "./logs/session/session.log"
    "/app/logs/session.log"
    "/var/log/dzfoot/session.log"
)
FOUND_SESSION=0
for LOG in "${SESSION_LOG_PATTERNS[@]}"; do
    if [ -f "$LOG" ]; then
        echo "--- Backend Session Log ($LOG) ---"
        grep -E "BOT_IN.*${ROOM_ID}|FORWARD.*${ROOM_ID}" "$LOG" | tail -n 20
        echo ""
        FOUND_SESSION=1
        break
    fi
done

if [ "$FOUND_SESSION" -eq 0 ]; then
    # Try docker logs as fallback
    SESSION_CONTAINER=$(docker ps --filter "name=session" --format "{{.Names}}" 2>/dev/null | head -n1)
    if [ -n "$SESSION_CONTAINER" ]; then
        echo "--- Backend Session Docker Log ($SESSION_CONTAINER) ---"
        docker logs "$SESSION_CONTAINER" 2>&1 | grep -E "BOT_IN.*${ROOM_ID}|FORWARD.*${ROOM_ID}" | tail -n 20
        echo ""
        FOUND_SESSION=1
    fi
fi

if [ "$FOUND_SESSION" -eq 0 ]; then
    echo "--- Backend Session Log NOT FOUND ---"
    echo "  Tried local files: ${SESSION_LOG_PATTERNS[*]}"
    echo "  Tried Docker containers matching 'session'"
    echo ""
fi

echo "=== End of Trace ==="
