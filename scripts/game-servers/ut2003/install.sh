#!/usr/bin/env bash
# (Re)install the UT2003 2225 server on the dev host from the STAGED tree.
#
#   bash scripts/game-servers/ut2003/install.sh            # copy + ini + unit
#   bash scripts/game-servers/ut2003/install.sh --no-copy  # ini + unit only
#
# Idempotent. Copies ONLY what a server loads (System, Maps, Textures,
# StaticMeshes, Animations, Sounds, KarmaData - ~2.6 GB; no Music, Help,
# Benchmark, Web or ForceFeedback) from the read-only /mnt mount to
# ~/ut2003-server, verifies the copy by FILE COUNT AND BYTE TOTAL per
# directory (a copy's exit code has lied here before), derives
# System/UT2003Server.ini from the staged UT2003.ini, and (re)starts the unit.
# Re-run it after the staged tree changes: the server must carry the SAME
# packages as the clients.
#
# Do NOT size the staged tree with plain `du`: the CIFS mount reports 128 MiB
# allocated for EVERY file (blocks=262144), so `du` says 106 GB for a 2.6 GB
# tree. Use `du --apparent-size`.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SRC:-/mnt/retro-share/Files/Games-Library/UT2003}"
DST="${DST:-$HOME/ut2003-server}"
UNITS="$HOME/.config/systemd/user"
DIRS="System Maps Textures StaticMeshes Animations Sounds KarmaData"

if [ "${1:-}" != "--no-copy" ]; then
    [ -f "$SRC/System/UCC.exe" ] || { echo "FAIL: no $SRC/System/UCC.exe (share not mounted?)"; exit 1; }
    mkdir -p "$DST"
    for d in $DIRS; do rsync -a "$SRC/$d" "$DST/"; done
    bad=0
    for d in $DIRS; do
        s="$(find "$SRC/$d" -type f -printf '%s\n' | awk '{n++;t+=$1}END{printf "%d/%d", n, t}')"
        # The destination System/ also holds files the server writes (logs,
        # UT2003Server.ini), so count only the names the source has.
        t="$(cd "$SRC/$d" && find . -type f -printf '%P\n' | (cd "$DST/$d" && xargs -d '\n' stat -c %s 2>/dev/null) | awk '{n++; t+=$1}END{printf "%d/%d", n, t}')"
        if [ "$s" != "$t" ]; then
            echo "FAIL: $d copied as $t (files/bytes), source is $s"; bad=1
        else
            echo "ok   $d $s"
        fi
    done
    [ "$bad" = 0 ] || exit 1
fi

# The rotation must name maps the tree really has.
for m in $(python3 -c "import sys; sys.path.insert(0, '$HERE'); import make_server_ini as m; print(' '.join(m.DM_ROTATION))"); do
    [ -f "$DST/Maps/$m.ut2" ] || { echo "FAIL: rotation map $m.ut2 is not in $DST/Maps"; exit 1; }
done

python3 "$HERE/make_server_ini.py" "$DST/System/UT2003.ini" "$DST/System/UT2003Server.ini"
mkdir -p "$DST/_run"
install -m 0755 "$HERE/entry.sh" "$HERE/run-ut2003-server.sh" "$DST/_run/"

mkdir -p "$UNITS"
install -m 0644 "$HERE/../units/ut2003-server.service" "$UNITS/"
systemctl --user daemon-reload
systemctl --user enable ut2003-server.service
systemctl --user restart ut2003-server.service
echo "ut2003-server: $(systemctl --user is-active ut2003-server) / $(systemctl --user is-enabled ut2003-server)"
echo "Now verify the post-condition, not the unit state (it takes ~30 s to load):"
echo "  RETRO_GAMESERVER_HOST=127.0.0.1 python3 scripts/game-servers/gameservers.py | grep UT2003"
