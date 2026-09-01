#!/bin/bash
# Container entrypoint for the DOOM 3 dedicated server.
#
# WHY WINE: id shipped a Linux Doom 3 server only as a 32-bit 1.3.1 blob whose
# libstdc++/SDL deps this host does not carry, and dhewm3 -- the maintained
# port -- is NOT network compatible with retail 1.3.  The staged fleet client
# IS retail 1.3 (Doom3/version.inf: ExtVersion=1.3).  DOOM3DED.exe from the
# same staged tree is therefore the ONLY server binary guaranteed to speak the
# protocol our clients speak, and the host already runs two Windows servers
# this way (Descent 3, Far Cry).
#
# `wine` RETURNS AS SOON AS WINESERVER OWNS THE PROCESS -- see the Descent 3
# entry script for the failure that causes.  Block on `wineserver -w`.
export WINEPREFIX="${WINEPREFIX:-/tmp/wp}"
export WINEDEBUG="${WINEDEBUG:--all}"
D3_MAP="${D3_MAP:-game/mp/d3dm1}"
D3_NAME="${D3_NAME:-NSC Retro Fleet Arena (DOOM 3)}"
D3_MAXPLAYERS="${D3_MAXPLAYERS:-4}"
D3_GAMETYPE="${D3_GAMETYPE:-Deathmatch}"

echo "[entry] DISPLAY=$DISPLAY map=$D3_MAP"
wineboot -i
cd /game || exit 1
# si_* are the SERVER info cvars id Tech 4 reads at +spawnServer time.
# net_serverDedicated 2 means "dedicated, no client, no rendering".
wine DOOM3DED.exe \
    +set net_serverDedicated 2 \
    +set si_name "$D3_NAME" \
    +set si_map "$D3_MAP" \
    +set si_gameType "$D3_GAMETYPE" \
    +set si_maxPlayers "$D3_MAXPLAYERS" \
    +set net_LANServer 1 \
    +set si_pure 0 \
    +set g_gameReviewPause 5 \
    +spawnServer &
echo "[entry] wine pid $!"
wineserver -w
echo "[entry] wineserver -w returned $?"
