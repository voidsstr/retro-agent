#!/bin/sh
# run-css-server.sh - start srcds on the host's CURRENT LAN address.
#
# srcds must bind the LAN address, never 0.0.0.0 (scripts/game-servers/README.md),
# and that address is DHCP's: the host moved from .132 to .196 on 2026-09-26 and a
# unit pinned to -ip 192.168.1.132 made srcds exit 100 at every start ("Couldn't
# allocate any server IP port"), 429 restarts, while HLDS in the same state quietly
# fell back to 0.0.0.0. The source address of the route to the fleet is the answer.
IP=$(ip -4 route get 192.168.1.1 | awk '{for (i = 1; i <= NF; i++) if ($i == "src") print $(i + 1)}')
if [ -z "$IP" ]; then
    echo "run-css-server: no route to the fleet LAN - not starting" >&2
    exit 1
fi
echo "run-css-server: binding $IP"
cd /home/voidsstr/css-server || exit 1
exec ./srcds_run -game cstrike -console -usercon -insecure -norestart -ip "$IP" \
    -port 27025 +tv_port 27035 +sv_lan 1 +maxplayers 16 +map de_dust2 +exec server.cfg
