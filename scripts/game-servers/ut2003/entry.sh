#!/bin/bash
# Unreal Tournament 2003 dedicated server, under Wine in a container.
#
# WHY THE STAGED UCC.exe UNDER WINE: the fleet's staged UT2003 tree is build
# 2225 (the last patch), and a UE2 client joins only a server of the same
# build -- the native browser reply carries net version 121 for UT2003 2225
# and 128 for UT2004, and the package GUIDs must match. Epic's Linux
# ut2003-lnxded build is not on the share, so the only server guaranteed to
# match the staged client is the staged tree's OWN System\UCC.exe, run over
# its own packages. Same shape as unrealgold/ (UE1, also staged UCC.exe).
#
# UCC.exe is a CONSOLE program: no window, no X display, no xvfb-run. A fatal
# error then exits (and systemd restarts it) instead of parking the server
# behind a dialog nobody will ever see. A dead server must LOOK dead.
#
# `wine` RETURNS AS SOON AS WINESERVER OWNS THE PROCESS, so background UCC
# and block on `wineserver -w` (see scripts/game-servers/README.md, Descent 3).
#
# Ports (UDP): game UT3_PORT (7757), native browser query 7758 (game+1),
# LAN responder 10777 (LANServerPort, shared with UT2004 -- see README.md).
export WINEPREFIX="${WINEPREFIX:-/tmp/wp}"
export WINEDEBUG="${WINEDEBUG:--all}"
UT3_MAP="${UT3_MAP:-DM-Antalus}"
UT3_GAME="${UT3_GAME:-XGame.xDeathMatch}"
UT3_MINPLAYERS="${UT3_MINPLAYERS:-6}"
UT3_MAXPLAYERS="${UT3_MAXPLAYERS:-12}"
UT3_PORT="${UT3_PORT:-7757}"

echo "[entry] $(date -Is) map=$UT3_MAP game=$UT3_GAME port=$UT3_PORT uid=$(id -u)"
# The container runs as the tree's owner and wine refuses a prefix inside a
# directory that uid does not own -- create the prefix directory itself first.
mkdir -p "$WINEPREFIX"
wineboot -i >/dev/null 2>&1
cd /game/System || exit 1

wine UCC.exe server \
    "$UT3_MAP?game=$UT3_GAME?MinPlayers=$UT3_MINPLAYERS?MaxPlayers=$UT3_MAXPLAYERS" \
    ini=UT2003Server.ini log=server-ucc.log -port="$UT3_PORT" &
echo "[entry] wine pid $!"
wineserver -w
echo "[entry] $(date -Is) wineserver -w returned $? - UCC is gone"
