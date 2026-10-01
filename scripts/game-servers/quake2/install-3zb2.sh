#!/usr/bin/env bash
# Install 3rd Zigock Bot II (Yamagi port) as the quake2-server's game module.
#
#   install-3zb2.sh [q2dir] [homecfgdir]
#
#   q2dir       the server's -datadir / WorkingDirectory (default ~/q2-server)
#   homecfgdir  Yamagi's home config dir, which SHADOWS q2dir's server.cfg
#               (default ~/.yq2/baseq2 - see the skill's "config shadowing")
#
# Server-side only, and clients stay in BASEQ2. Upstream 3zb2 finds its data
# through the "game" cvar, which would make the server run as gamedir 3zb2 and
# switch every connecting client to a mod directory it does not have.
# 3zb2-fleet.patch moves that to its own cvar (zb_path, default "3zb2", a path
# relative to the server's working directory) and turns the Windows-only
# backslash paths into forward slashes. The game.so is dropped into
# <q2dir>/baseq2/, and the unit runs a copy of the Yamagi binary from <q2dir>
# so that directory is the one Yamagi loads game.so from (it prefers the dir
# beside its own binary over -datadir; quake2-server-3zb2.conf).
#
# The bot data (3ZBConfig.cfg, 3ZBMAPS.LST, chdtm/ route files - one per stock
# DM map, q2dm1..8 included) goes to <q2dir>/3zb2/, which is NOT a gamedir.
# 3zb2's assets pak (pak6.pak) is NOT installed: in deathmatch with zigmode 0
# nothing in it is referenced, and installing it would only invite a client to
# want it.
#
# Source: https://github.com/yquake2/3zb2 (GPL), pinned to the commit below.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
Q2="${1:-$HOME/q2-server}"
HOMECFG="${2:-$HOME/.yq2/baseq2}"
REPO=https://github.com/yquake2/3zb2.git
COMMIT=4283a076f3a31e01ab38c60c93e513b639aa4b4a
BUILD="${BUILD_DIR:-$HOME/.cache/game-servers/3zb2}"

[ -d "$Q2/baseq2" ] || { echo "no $Q2/baseq2" >&2; exit 1; }
if [ ! -d "$BUILD/.git" ]; then
  mkdir -p "$(dirname "$BUILD")"
  git clone -q "$REPO" "$BUILD"
fi
git -C "$BUILD" fetch -q origin
git -C "$BUILD" checkout -q -f "$COMMIT"
git -C "$BUILD" clean -qfdx
git -C "$BUILD" apply "$HERE/3zb2-fleet.patch"
make -C "$BUILD" -j"$(nproc)" >/dev/null
test -f "$BUILD/release/game.so"

echo "== installing game.so into $Q2/baseq2 (Yamagi's packaged one is left alone)"
[ -f "$Q2/baseq2/game.so" ] && ! cmp -s "$BUILD/release/game.so" "$Q2/baseq2/game.so" \
  && cp -p "$Q2/baseq2/game.so" "$Q2/baseq2/game.so.prev"
install -m 755 "$BUILD/release/game.so" "$Q2/baseq2/game.so"

echo "== bot data into $Q2/3zb2"
mkdir -p "$Q2/3zb2/chdtm" "$Q2/3zb2/chctf"
unzip -oqj "$BUILD/misc/assets.zip" '3zb2/3ZBConfig.cfg' -d "$Q2/3zb2"
# The code opens 3ZBMAPS.LST in capitals; the repo ships 3ZBMaps.lst.
install -m 644 "$BUILD/misc/3ZBMaps.lst" "$Q2/3zb2/3ZBMAPS.LST"
cp -p "$BUILD"/misc/chdtm/*.chn "$Q2/3zb2/chdtm/"
cp -p "$BUILD"/misc/chctf/* "$Q2/3zb2/chctf/" 2>/dev/null || true
# Append the fleet botlist once (CRLF like the rest of the file).
if ! grep -q '^\[fleet\]' "$Q2/3zb2/3ZBConfig.cfg"; then
  printf '\r\n' >> "$Q2/3zb2/3ZBConfig.cfg"
  sed 's/$/\r/' "$HERE/3zb2-fleet-bots.cfg" >> "$Q2/3zb2/3ZBConfig.cfg"
fi

echo "== private copy of the Yamagi binary + unit drop-in (see quake2-server-3zb2.conf)"
install -m 755 /usr/lib/yamagi-quake2/quake2 "$Q2/quake2"
DROPIN="$HOME/.config/systemd/user/quake2-server.service.d"
mkdir -p "$DROPIN"
sed "s#/home/voidsstr/q2-server#$Q2#g" "$HERE/quake2-server-3zb2.conf" > "$DROPIN/3zb2.conf"
systemctl --user daemon-reload

echo "== server.cfg (both copies - Yamagi reads $HOMECFG first)"
for cfg in "$HOMECFG/server.cfg" "$Q2/baseq2/server.cfg"; do
  [ -f "$cfg" ] || continue
  install -m 644 "$HERE/server.cfg" "$cfg"
done
echo "== done. systemctl --user restart quake2-server"
