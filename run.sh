#!/bin/bash

# Run GF Server for a single match
# Example usage: ./run.sh match-abc team-a team-b stadium-1 600

ROOM_ID=${1:-"match-$(uuidgen)"}
TEAM_A=${2:-"default-a"}
TEAM_B=${3:-"default-b"}
STADIUM=${4:-"default-stadium"}
DURATION=${5:-600}

LK_URL=${LIVEKIT_URL:-"wss://your-livekit.com"}
LK_TOKEN=${LIVEKIT_TOKEN:-""}
STATS_URL=${STATS_SERVICE_URL:-"http://stats:8000"}

./gf_server \
    --room-id="$ROOM_ID" \
    --team-a="$TEAM_A" \
    --team-b="$TEAM_B" \
    --stadium="$STADIUM" \
    --duration="$DURATION" \
    --livekit-url="$LK_URL" \
    --livekit-token="$LK_TOKEN" \
    --stats-url="$STATS_URL"
