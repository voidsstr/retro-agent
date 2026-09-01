#!/bin/bash
# Serious Sam: The First Encounter dedicated server, under Wine in a container.
#
# Croteam shipped no Linux server for Serious Engine 1, and DedicatedServer.exe
# is in the staged tree already - so it is guaranteed to speak exactly the
# protocol the fleet's own SeriousSam.exe speaks.
#
# DedicatedServer.exe has NO CD CHECK. Only SeriousSam.exe walks the drive
# letters for a DRIVE_CDROM volume holding <drive>:\Install\Bin\SeriousSam.exe;
# the server binary does not, which is why .123 (no optical drive, no mounter)
# could host this title while being unable to play it. That is what makes it
# hostable from this Linux box at all.
#
# `wine` returns as soon as wineserver owns the process - block on wineserver.
export WINEPREFIX="${WINEPREFIX:-/tmp/wp}"
export WINEDEBUG="${WINEDEBUG:--all}"
SS_CONFIG="${SS_CONFIG:-NSCFleet}"
echo "[entry] DISPLAY=$DISPLAY config=$SS_CONFIG"
wineboot -i
cd /game || exit 1
# The argument is the SUBDIRECTORY NAME under Scripts\Dedicated\, not a path.
wine Bin/DedicatedServer.exe "$SS_CONFIG" &
echo "[entry] wine pid $!"
wineserver -w
echo "[entry] wineserver -w returned $?"
