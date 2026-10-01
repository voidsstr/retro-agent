#!/bin/bash
# Carmageddon 2 LAN host - runs INSIDE the container (see run-carmageddon2-server.sh).
#
# Carmageddon 2 has no dedicated server: one player HOSTS a network game and
# the rest join. This starts the software-rendered build (Carma2_SW.exe - the
# host has no Glide and no Direct3D in here) and lets host.py drive it into
# the "LOADING STATUS" lobby, where fleet boxes see it in their JOIN list.
export WINEPREFIX=/wp WINEDEBUG=-all
# IPXWrapper (IPX over UDP 54792) ships as game-local Winsock stubs; Wine
# loads its own builtin wsock32 unless told to prefer the native DLLs.
export WINEDLLOVERRIDES="wsock32,mswsock,dpwsockx=n,b"
# Pin IPXWrapper's UDP socket to the wired NIC (bindiface.c says why). In
# Wine 8 the bind is executed by the 64-bit wineserver, which is where this
# has to load; the 32-bit game process prints one "wrong ELF class" line.
export LD_PRELOAD=/game/_run/bindiface.so C2_BIND_LOG=/game/_run/bindiface.log

wineboot -i >/dev/null 2>&1
# IPXWrapper's interface table (generated per start by the run script: the
# docker bridges come and go) and the title's own registry seed.
wine regedit /s 'Z:\game\_run\ipxwrapper.reg'
wine regedit /s 'Z:\game\game\install.reg'
# Read it back - never trust regedit's silence.
if wine reg query 'HKCU\Software\IPXWrapper' /v primary 2>/dev/null | tr -d '\r' | grep -qi primary; then
    echo "[entry] IPXWrapper config in the prefix: primary interface set"
else
    echo "[entry] *** IPXWrapper config NOT in the prefix - the game may host on the wrong NIC ***"
fi

rm -f /game/_run/state.json /game/game/ipxwrapper.log /game/_run/bindiface.log
cd /game/game || exit 1
wine explorer /desktop=carma,640x480 Carma2_SW.exe &
echo "[entry] Carma2_SW.exe launched"

python3 /game/_run/host.py &
HOST=$!
wineserver -w &
WS=$!
wait -n "$HOST" "$WS"
rc=$?
if ! kill -0 "$HOST" 2>/dev/null; then
    echo "[entry] host.py exited ($rc) - stopping Wine so the unit restarts us"
    wineserver -k
else
    echo "[entry] Wine exited ($rc) - the game is gone"
    kill "$HOST" 2>/dev/null
fi
exit 1
