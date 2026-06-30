#!/usr/bin/env bash
# Supervisor fallback for the retro chat brain (use the systemd unit if you can).
# Restarts retro_chat_brain.py on crash with a short backoff.
#
#   nohup bash scripts/retro_chat_brain_supervisor.sh > /tmp/retro-chat/brain-sup.log 2>&1 &
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
PY="$REPO/scripts/.brain-venv/bin/python"
BRAIN="$REPO/scripts/retro_chat_brain.py"
export PATH="$HOME/.local/bin:/usr/local/bin:$PATH"

cd "$REPO" || exit 1
while true; do
    echo "$(date '+%F %T') starting brain"
    "$PY" "$BRAIN"
    code=$?
    echo "$(date '+%F %T') brain exited ($code); restarting in 3s"
    sleep 3
done
