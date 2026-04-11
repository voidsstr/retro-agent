#!/bin/bash
# chat_status.sh - report retro chat daemon + processor health
#
# Used by Claude Code on session startup to tell the user whether the
# chat infrastructure is healthy and what to do if not.
#
# Output is plain text suitable for Claude to read and relay to the user.

set -e

ROOT=/tmp/retro-chat
DAEMON_PID_FILE="$ROOT/daemon.pid"
DAEMON_LOG="$ROOT/daemon.log"
PROCESSOR_HEARTBEAT="$ROOT/processor.heartbeat"
INBOX="$ROOT/inbox"
OUTBOX="$ROOT/outbox"
HISTORY="$ROOT/history"

echo "=== Retro Chat Status ==="
echo

# 1. Daemon
if [ -f "$DAEMON_PID_FILE" ]; then
    PID=$(cat "$DAEMON_PID_FILE")
    if kill -0 "$PID" 2>/dev/null; then
        echo "  daemon: RUNNING (pid $PID)"
        # Show claimed agents from log
        if [ -f "$DAEMON_LOG" ]; then
            CLAIMED=$(grep -o "claimed [0-9]* agents:.*" "$DAEMON_LOG" | tail -1)
            if [ -n "$CLAIMED" ]; then
                echo "  agents: $CLAIMED"
            fi
        fi
    else
        echo "  daemon: STALE PID FILE (process $PID not running)"
        echo "  fix:    bash /home/voidsstr/development/retro-agent/scripts/restart_daemon.sh"
    fi
else
    echo "  daemon: NOT RUNNING"
    echo "  start:  bash /home/voidsstr/development/retro-agent/scripts/restart_daemon.sh"
fi

# 2. Processor (detected via heartbeat file)
if [ -f "$PROCESSOR_HEARTBEAT" ]; then
    HB_AGE=$(( $(date +%s) - $(stat -c%Y "$PROCESSOR_HEARTBEAT" 2>/dev/null || stat -f%m "$PROCESSOR_HEARTBEAT") ))
    if [ "$HB_AGE" -lt 120 ]; then
        echo "  processor: ALIVE (last heartbeat ${HB_AGE}s ago)"
    else
        echo "  processor: STALE (last heartbeat ${HB_AGE}s ago, likely dead)"
        echo "  start:    ask 'start the chat processor'"
    fi
else
    echo "  processor: NOT RUNNING (no heartbeat file)"
    echo "  start:     ask 'start the chat processor'"
fi

# 3. Inbox / outbox state
if [ -d "$INBOX" ]; then
    INBOX_COUNT=$(ls -1 "$INBOX"/*.json 2>/dev/null | wc -l)
    echo "  inbox: $INBOX_COUNT pending prompts"
fi
if [ -d "$OUTBOX" ]; then
    OUTBOX_COUNT=$(ls -1 "$OUTBOX"/*.json 2>/dev/null | wc -l)
    echo "  outbox: $OUTBOX_COUNT queued responses"
fi

# 4. Recent activity from history
if [ -d "$HISTORY" ]; then
    HOST_COUNT=$(ls -1d "$HISTORY"/*/ 2>/dev/null | wc -l)
    if [ "$HOST_COUNT" -gt 0 ]; then
        TOTAL_PROMPTS=$(find "$HISTORY" -name "prompt-*.txt" 2>/dev/null | wc -l)
        echo "  history: $TOTAL_PROMPTS prompts across $HOST_COUNT host(s)"
    fi
fi

echo
echo "Daemon log: $DAEMON_LOG"
