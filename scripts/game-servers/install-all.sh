#!/usr/bin/env bash
# Install all three game servers (UT2004 + Quake 2 + OpenArena/Q3) in one shot.
# Each sub-script is idempotent, so re-running this is safe.
set -euo pipefail

HERE="$(dirname "$(readlink -f "$0")")"

bash "$HERE/install-ut2004-server.sh"
bash "$HERE/install-quake2-server.sh"
bash "$HERE/install-openarena-server.sh"

echo
echo "All three servers installed. Status:"
systemctl --user status ut2004-server.service quake2-server.service openarena-server.service --no-pager -n 3 | grep -E '●|Active:' || true
cat <<'TAIL'

Router port-forwards needed (UDP, all → this host's wired LAN IP):
  7777, 7778, 7787     UT2004 (game + browser + gamespy query)
  27910                Quake 2
  27960                OpenArena / Q3

External reachability test (run from a different network / mobile hotspot):
  nc -u <your.public.ip> 7787     # UT2004 gamespy - should reply with server info
  echo -ne '\xff\xff\xff\xffgetstatus' | nc -u -w3 <your.public.ip> 27960

Masters will re-probe and list the servers within 15–30 minutes.
TAIL
