#!/bin/bash
# Unreal Gold 226 dedicated server, under Wine in a container.
#
# WHY 226 UNDER WINE AND NOT THE OLDUNREAL 227k LINUX BUILD: the fleet's staged
# UnrealGold tree is retail Unreal Gold patched to 226 Final, and a 226 client
# CANNOT join a 227 server. 227's packages are a later generation, so the
# client aborts on connect with
#     Warning: Failed to load 'UnrealI': Package 'UnrealI' version mismatch
# even though the 227k server advertises mingamever\224 (measured on .124,
# 2026-09-28). Unreal 226 has no Linux build, so the only server the staged
# client can join is the staged tree's OWN System\UCC.exe -- run here, over the
# staged tree's own packages, which is also what makes every package GUID
# match the client's by construction.
#
# UCC.exe is a CONSOLE program: it draws no window and needs no X display, so
# there is no xvfb-run here (unlike Deus Ex, whose server is the game exe).
# With no display, a fatal appErrorf cannot park the server behind a dialog
# box that nobody will ever see -- it exits, the container exits, and systemd
# restarts it. A server that is dead must LOOK dead.
#
# `wine` RETURNS AS SOON AS WINESERVER OWNS THE PROCESS (see the Descent 3
# note in scripts/game-servers/README.md), so background it and block on
# `wineserver -w`.
#
# Ports: game UG_PORT (7807), GameSpy query = the next free port (7808), LAN
# beacon 7775. The query port is not configured anywhere: UdpServerQuery binds
# "the game port, or the next one free", so if 7808 is ever taken it will
# silently move to 7809 and the probe will read the server as down.
export WINEPREFIX="${WINEPREFIX:-/tmp/wp}"
export WINEDEBUG="${WINEDEBUG:--all}"
UG_MAP="${UG_MAP:-DmDeck16.unr}"
UG_GAME="${UG_GAME:-UnrealShare.DeathMatchGame}"
UG_MAXPLAYERS="${UG_MAXPLAYERS:-12}"
UG_PORT="${UG_PORT:-7807}"

echo "[entry] $(date -Is) map=$UG_MAP game=$UG_GAME port=$UG_PORT uid=$(id -u)"
# The container runs as the tree's owner (run-ug-server.sh --user), and wine
# refuses to create a prefix inside a directory that uid does not own
# ("'/tmp' is not owned by you") -- make the prefix directory itself first.
mkdir -p "$WINEPREFIX"
wineboot -i >/dev/null 2>&1
cd /game/System || exit 1

# UE1 writes Running.ini while it runs and deletes it on a clean exit. A killed
# server leaves it behind; UCC ignores it, but never ship a tree that carries it.
rm -f Running.ini running.ini

wine UCC.exe server "$UG_MAP?game=$UG_GAME?maxplayers=$UG_MAXPLAYERS" \
    -ini=UnrealServer.ini -port="$UG_PORT" &
echo "[entry] wine pid $!"
wineserver -w
echo "[entry] $(date -Is) wineserver -w returned $? - UCC is gone"
