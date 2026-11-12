#!/bin/bash
# Read-only watch of .133: port states + protocol-level agent check every 30 s, up to 12 min.
IP=192.168.1.133
LOG=/home/voidsstr/development/retro-agent/.claude/evidence-icons/192.168.1.133/watch.log
end=$(( $(date +%s) + 720 ))
n=0
while [ $(date +%s) -lt $end ]; do
  n=$((n+1))
  ports=""
  for p in 135 139 445 9897 9898; do
    if timeout 4 bash -c "exec 3<>/dev/tcp/$IP/$p" 2>/dev/null; then ports="$ports $p:open"; else ports="$ports $p:closed"; fi
  done
  echo "[$(date +%T)] probe $n ports:$ports" >> "$LOG"
  out=$(timeout 60 python3 /home/voidsstr/development/retro-agent/.claude/evidence-icons/boxcheck.py $IP status 2>&1)
  rc=$?
  echo "[$(date +%T)] probe $n boxcheck rc=$rc" >> "$LOG"
  echo "$out" | head -c 3000 >> "$LOG"; echo >> "$LOG"
  if [ $rc -eq 0 ]; then echo "[$(date +%T)] AGENT UP" >> "$LOG"; exit 0; fi
  sleep 30
done
echo "[$(date +%T)] WINDOW EXPIRED - agent never answered" >> "$LOG"
exit 3
