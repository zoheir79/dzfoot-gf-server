#!/bin/bash
set -e
set -o pipefail
# Wrapper to duplicate gf_server stdout/stderr to both container stdout and a host log file.
# This keeps logs visible via 'docker logs' AND persists a copy on the host.
# Usage: run_logged.sh <gf_server_args...>
# Set GF_LOG_FILE env var to override the default log path (e.g. per-room log).

LOGFILE="${GF_LOG_FILE:-/var/log/dzfoot/gf/gf_server.log}"
mkdir -p "$(dirname "$LOGFILE")"
gf_server "$@" 2>&1 | tee -a "$LOGFILE"
