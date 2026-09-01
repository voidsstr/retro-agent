#!/bin/bash
# Deus Ex multiplayer dedicated server, under Wine in a container.
#
# There is no ucc.exe in the Deus Ex tree and no Linux build of the engine, so
# the server is DEUSEX.EXE itself in `-server` mode - the form the staged
# tree's README-FLEET proved on two boxes.  It runs HEADLESS and draws no
# window, so "is there a window" tells you nothing; the post-condition is the
# GameSpy reply on the QUERY port.
#
# *** THE QUERY PORT IS THE GAME PORT + 1: 7790 -> 7791. ***
# Probing 7776/7777 times out and reads as "no server at all".
export WINEPREFIX="${WINEPREFIX:-/tmp/wp}"
export WINEDEBUG="${WINEDEBUG:--all}"
DX_MAP="${DX_MAP:-DXMP_Cathedral.dx}"
echo "[entry] DISPLAY=$DISPLAY map=$DX_MAP"
wineboot -i
cd /game/System || exit 1
wine DEUSEX.EXE "$DX_MAP?game=DeusEx.DeusExMPGame" -server &
echo "[entry] wine pid $!"
wineserver -w
echo "[entry] wineserver -w returned $?"
