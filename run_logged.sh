#!/bin/bash
set -e
# Wrapper to redirect gf_server stdout/stderr to a persistent log file
# Usage: run_logged.sh <gf_server_args...>
# Set GF_LOG_FILE env var to override the default log path (e.g. per-room log).

mkdir -p /app/logs
LOGFILE="${GF_LOG_FILE:-/app/logs/gf_server.log}"
exec gf_server "$@" >> "$LOGFILE" 2>&1
