#!/bin/bash
# daemon_supervisor.sh - keep retro_chat_daemon.py running, restart on crash
#
# Run via restart_daemon.sh (which backgrounds it). The supervisor loops
# forever, restarting the daemon if it exits. Backoff if it crashes
# repeatedly to avoid CPU spinning.

set -e

DAEMON_PY=/home/voidsstr/development/nsc-assistant/agent/tools/retro_chat_daemon.py
DAEMON_DIR=/home/voidsstr/development/nsc-assistant

cd "$DAEMON_DIR"

declare -a recent_restarts
backoff=1

while true; do
    echo "[supervisor] starting daemon..."
    python3 "$DAEMON_PY" || true
    EXIT_CODE=$?
    NOW=$(date +%s)
    echo "[supervisor] daemon exited with code $EXIT_CODE at $NOW"

    recent_restarts+=("$NOW")
    new=()
    for t in "${recent_restarts[@]}"; do
        if [ "$((NOW - t))" -lt 60 ]; then
            new+=("$t")
        fi
    done
    recent_restarts=("${new[@]}")

    if [ "${#recent_restarts[@]}" -gt 5 ]; then
        echo "[supervisor] crash loop detected (${#recent_restarts[@]} restarts in 60s), backoff ${backoff}s"
        sleep $backoff
        if [ "$backoff" -lt 60 ]; then
            backoff=$((backoff * 2))
        fi
    else
        backoff=1
        sleep 2
    fi
done
