#!/bin/bash
# =============================================================================
# log_gf.sh — Find and tail GF Server logs regardless of deployment mode
# =============================================================================

ROOM_ID="${1:-}"
MODE="${2:-auto}"   # auto | docker | native

if [ -z "$ROOM_ID" ]; then
    echo "Usage: $0 <room_id> [mode]"
    echo ""
    echo "Modes:"
    echo "  auto   — Try Docker first, then native (default)"
    echo "  docker — Force Docker container logs"
    echo "  native — Force native /var/log/dzfoot/gf/ logs"
    echo ""
    echo "Examples:"
    echo "  $0 abc-123-def-456"
    echo "  $0 abc-123-def-456 docker"
    echo "  $0 abc-123-def-456 native"
    exit 1
fi

GF_LOG="/var/log/dzfoot/gf/gf_${ROOM_ID}.log"

if [ "$MODE" = "auto" ] || [ "$MODE" = "docker" ]; then
    # Try Docker first
    CONTAINER=$(docker ps --filter "name=gf-${ROOM_ID}" --format "{{.ID}}" 2>/dev/null | head -n1)
    if [ -n "$CONTAINER" ]; then
        echo "=== Docker container found: $CONTAINER ==="
        echo "Tailing logs (Ctrl+C to stop)..."
        docker logs -f "$CONTAINER"
        exit 0
    elif [ "$MODE" = "docker" ]; then
        echo "ERROR: Docker container for room $ROOM_ID not found."
        docker ps --filter "name=gf-" --format "table {{.Names}}\t{{.Status}}"
        exit 1
    fi
fi

if [ "$MODE" = "auto" ] || [ "$MODE" = "native" ]; then
    if [ -f "$GF_LOG" ]; then
        echo "=== Native log found: $GF_LOG ==="
        echo "Tailing logs (Ctrl+C to stop)..."
        tail -f "$GF_LOG"
        exit 0
    elif [ "$MODE" = "native" ]; then
        echo "ERROR: Native log not found: $GF_LOG"
        echo "Available GF logs in /var/log/dzfoot/gf/:"
        ls -la /var/log/dzfoot/gf/ 2>/dev/null || echo "  Directory does not exist!"
        exit 1
    fi
fi

# auto mode and neither found
echo "ERROR: No GF logs found for room $ROOM_ID"
echo ""
echo "Tried:"
echo "  Docker container: gf-${ROOM_ID}"
echo "  Native log:       $GF_LOG"
echo ""
echo "Running containers:"
docker ps --filter "name=gf-" --format "table {{.Names}}\t{{.Status}}" 2>/dev/null || echo "  (none or Docker not available)"
echo ""
echo "Native log directory:"
ls -la /var/log/dzfoot/gf/ 2>/dev/null || echo "  Directory does not exist!"
