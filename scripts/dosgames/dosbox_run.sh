#!/bin/bash
# dosbox_run.sh — run a DOS command headlessly in DOSBox-X (under Wine) with a
# host directory mounted as C:. The dev-host DOS test loop for dosgame.exe.
#
#   dosbox_run.sh <host-dir-as-C> <dos-command> [extra -c cmds...]
#
# Notes (hard-won, see toolchain-3dfx/README.md):
#   - wine output must go to a FILE, never a pipe (services.exe inherits fds)
#   - always ulimit -f + timeout every wine call
#   - dosbox-staging Linux build hard-requires GLX (absent on :20/Xvfb) —
#     that's why we run the DOSBox-X mingw build under Wine instead.
set -u
TD="$HOME/development/toolchain-dos"
WINE="$HOME/development/retro-3dfx/toolchain-3dfx/wine/bin/wine"
DBX="$TD/dosbox-x-win/mingw-build/mingw-sdl2/dosbox-x.exe"
export WINEPREFIX="$TD/wineprefix" WINEDEBUG=-all
# Private Xvfb — NEVER a real session display (:20 is a live desktop).
export DISPLAY="${DOSBOX_DISPLAY:-:77}"
if [ "$DISPLAY" = ":77" ] && ! [ -e /tmp/.X11-unix/X77 ]; then
    Xvfb :77 -screen 0 1280x960x24 >/dev/null 2>&1 &
    sleep 2
fi

HOSTDIR="$1"; shift
DOSCMD="$1"; shift
WINPATH="Z:$(echo "$HOSTDIR" | sed 's|/|\\|g')"
LOG="${DOSBOX_LOG:-$HOSTDIR/dosbox-run.log}"

ulimit -f 2000000
CONFARGS=(-defaultconf)
if [ -n "${DOSBOX_CONF:-}" ]; then
    CONFARGS=(-conf "Z:$(echo "$DOSBOX_CONF" | sed 's|/|\\|g')")
fi
timeout "${DOSBOX_TIMEOUT:-120}" "$WINE" "$DBX" "${CONFARGS[@]}" -fastlaunch -nomenu \
    -c "MOUNT C '$WINPATH'" -c "C:" -c "$DOSCMD" "$@" -c "exit" \
    > "$LOG" 2>&1
rc=$?
pkill -9 -f 'dosbox-x\.exe' 2>/dev/null
exit $rc
