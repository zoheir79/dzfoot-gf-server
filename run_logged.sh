#!/bin/bash
set -e
# Wrapper to redirect gf_server stdout/stderr to a persistent log file
# Usage: run_logged.sh <gf_server_args...>
# Set GF_LOG_FILE env var to override the default log path (e.g. per-room log).

LOGFILE="${GF_LOG_FILE:-/var/log/dzfoot/gf/gf_server.log}"
mkdir -p "$(dirname "$LOGFILE")"
exec gf_server "$@" >> "$LOGFILE" 2>&1
