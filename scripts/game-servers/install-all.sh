#!/usr/bin/env bash
# Install all four game servers (UT2004 + UT99 + Quake 2 + OpenArena/Q3).
# Each sub-script is idempotent, so re-running this is safe.
set -euo pipefail

HERE="$(dirname "$(readlink -f "$0")")"

bash "$HERE/install-ut2004-server.sh"
bash "$HERE/install-ut99-server.sh"
bash "$HERE/install-quake2-server.sh"
bash "$HERE/install-openarena-server.sh"

echo
echo "All four servers installed. Status:"
systemctl --user status \
    ut2004-server.service ut99-server.service \
    quake2-server.service openarena-server.service \
    --no-pager -n 3 2>/dev/null | grep -E '●|Active:' || true

cat <<'TAIL'

Router port-forwards needed (UDP, all → this host's wired LAN IP):
  7777, 7778, 7787     UT2004 (game + browser + gamespy query)
  7797, 7798           UT99 (game + query — shifted from default 7777)
  27910                Quake 2
  27960                OpenArena / Q3

External reachability test (run from a different network / mobile hotspot):
  printf '\\status\\' | nc -u -w3 <public.ip> 7787        # UT2004 gamespy
  printf '\\status\\' | nc -u -w3 <public.ip> 7798        # UT99 query
  printf '\xff\xff\xff\xffstatus\n' | nc -u -w3 <public.ip> 27910   # Q2
  printf '\xff\xff\xff\xffgetstatus' | nc -u -w3 <public.ip> 27960  # OpenArena

Masters will re-probe and list the servers within 15–30 minutes.
TAIL
