#!/bin/bash
# restart_daemon.sh - stop and restart the retro chat daemon
#
# Usage: bash /home/voidsstr/development/retro-agent/scripts/restart_daemon.sh
#
# Always uses the canonical daemon path in nsc-assistant/agent/tools/.

set -e

DAEMON_PY=/home/voidsstr/development/nsc-assistant/agent/tools/retro_chat_daemon.py
DAEMON_DIR=/home/voidsstr/development/nsc-assistant
LOG=/tmp/retro-chat/daemon.stdout

mkdir -p /tmp/retro-chat

# Stop any running daemon
python3 "$DAEMON_PY" --stop 2>&1 || true
sleep 1

# Kill any lingering python process running the daemon
pkill -f "retro_chat_daemon.py" 2>/dev/null || true
sleep 1

# Start fresh under the daemon supervisor (auto-restarts on crash)
SUPERVISOR=/home/voidsstr/development/retro-agent/scripts/daemon_supervisor.sh
if [ -f "$SUPERVISOR" ]; then
    cd "$DAEMON_DIR"
    nohup bash "$SUPERVISOR" > "$LOG" 2>&1 &
    echo "supervisor started (pid $!)"
else
    cd "$DAEMON_DIR"
    nohup python3 "$DAEMON_PY" > "$LOG" 2>&1 &
    echo "daemon started (pid $!)"
fi

sleep 3
python3 "$DAEMON_PY" --status
