#!/bin/sh
# run-css-server.sh - start srcds on the host's CURRENT LAN address.
#
# srcds must bind the LAN address, never 0.0.0.0 (scripts/game-servers/README.md),
# and that address is DHCP's: the host moved from .132 to .196 on 2026-09-26 and a
# unit pinned to -ip 192.168.1.132 made srcds exit 100 at every start ("Couldn't
# allocate any server IP port"), 429 restarts, while HLDS in the same state quietly
# fell back to 0.0.0.0. The source address of the route to the fleet is the answer.
# 2026-10-01: the route source is not enough either. With the wired NIC holding
# .132 AND .196 and the Wi-Fi up on .129, the route to the gateway left by Wi-Fi,
# srcds bound .129, and every fleet probe of .132:27025 read the server as down.
# So the fleet's published address wins whenever this host holds it.
FLEET_IP=${RETRO_GAMESERVER_HOST:-192.168.1.132}
if ip -4 -o addr show | awk '{print $4}' | cut -d/ -f1 | grep -qx "$FLEET_IP"; then
    IP=$FLEET_IP
else
    IP=$(ip -4 route get 192.168.1.1 | awk '{for (i = 1; i <= NF; i++) if ($i == "src") print $(i + 1)}')
fi
if [ -z "$IP" ]; then
    echo "run-css-server: no route to the fleet LAN - not starting" >&2
    exit 1
fi
echo "run-css-server: binding $IP"
cd /home/voidsstr/css-server || exit 1
exec ./srcds_run -game cstrike -console -usercon -insecure -norestart -ip "$IP" \
    -port 27025 +tv_port 27035 +sv_lan 1 +maxplayers 16 +map de_dust2 +exec server.cfg
