#!/usr/bin/env bash
# (Re)install the Unreal Gold 226 server on the dev host from the STAGED tree.
#
#   bash scripts/game-servers/unrealgold/install.sh            # copy + ini + unit
#   bash scripts/game-servers/unrealgold/install.sh --no-copy  # ini + unit only
#
# Idempotent. It copies the library's UnrealGold tree (read-only /mnt mount) to
# ~/unrealgold-server, verifies the copy by FILE COUNT AND BYTE TOTAL per
# directory (a copy's exit code has lied here before), derives the server ini
# from the staged Unreal.ini, and (re)starts the unit. Re-run it after the
# staged tree changes: the server must carry the SAME packages as the clients,
# or they are refused with "Package ... version mismatch".
#
# The retired OldUnreal 227k unit is installed as unrealgold227-server and left
# DISABLED -- it binds the same ports. See README.md in this directory.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SRC:-/mnt/retro-share/Files/Games-Library/UnrealGold}"
DST="${DST:-$HOME/unrealgold-server}"
UNITS="$HOME/.config/systemd/user"

if [ "${1:-}" != "--no-copy" ]; then
    [ -f "$SRC/System/UCC.exe" ] || { echo "FAIL: no $SRC/System/UCC.exe (share not mounted?)"; exit 1; }
    mkdir -p "$DST"
    # Help/ and Manual/ are documents; nothing a server loads.
    rsync -a --exclude 'Help/' --exclude 'Manual/' "$SRC/" "$DST/"
    bad=0
    for d in Maps Music Sounds System Textures; do
        s="$(find "$SRC/$d" -type f | wc -l)/$(find "$SRC/$d" -type f -printf '%s\n' | awk '{t+=$1}END{print t+0}')"
        # The destination System/ also holds files the server writes (ucc.log,
        # UnrealServer.ini), so count only the names the source has.
        t="$(cd "$SRC/$d" && find . -type f -printf '%P\n' | (cd "$DST/$d" && xargs -d '\n' stat -c %s 2>/dev/null) | awk '{n++; t+=$1}END{printf "%d/%d", n, t}')"
        if [ "$s" != "$t" ]; then
            echo "FAIL: $d copied as $t (files/bytes), source is $s"; bad=1
        else
            echo "ok   $d $s"
        fi
    done
    [ "$bad" = 0 ] || exit 1
fi

python3 "$HERE/make_server_ini.py" "$DST/System/Unreal.ini" "$DST/System/UnrealServer.ini"
mkdir -p "$DST/_run"
install -m 0755 "$HERE/entry.sh" "$HERE/run-ug-server.sh" "$DST/_run/"

mkdir -p "$UNITS"
install -m 0644 "$HERE/../units/unrealgold-server.service" "$UNITS/"
install -m 0644 "$HERE/../units/unrealgold227-server.service" "$UNITS/"
systemctl --user daemon-reload
systemctl --user disable --now unrealgold227-server.service 2>/dev/null || true
systemctl --user enable unrealgold-server.service
systemctl --user restart unrealgold-server.service
echo "unrealgold-server: $(systemctl --user is-active unrealgold-server) / $(systemctl --user is-enabled unrealgold-server)"
echo "Now verify the post-condition, not the unit state:"
echo "  RETRO_GAMESERVER_HOST=127.0.0.1 python3 scripts/game-servers/gameservers.py"
