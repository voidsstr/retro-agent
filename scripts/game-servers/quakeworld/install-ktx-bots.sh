#!/usr/bin/env bash
# Turn on KTX's built-in frogbots on the quakeworld-server (mvdsv + KTX 1.47).
#
#   install-ktx-bots.sh [qwdir] [q1dir]
#     qwdir  the mvdsv tree           (default ~/qw-server)
#     q1dir  where pak1.pak is found  (default ~/quake1-server, same licensed data)
#
# Server-side only: KTX's bots are mvdsv bot clients (*bot userinfo), and the
# deployed qwprogs.so was built with BOT_SUPPORT. What KTX needs and did not
# have:
#   * WAYPOINTS - bots/maps/<map>.bot in the gamedir. KTX ships them in its own
#     repo (resources/example-configs/ktx/bots/maps, GPL, 77 maps); no file =
#     "Map X not supported for bots" and no bot ever spawns.
#   * MAPS THAT HAVE WAYPOINTS - this tree had only the shareware pak0 (start,
#     e1m1-e1m8), and of those only e1m2 has a .bot file. pak1.pak (registered:
#     dm1-dm6) is copied from the quake1-server tree, so dm3, dm4 and dm6 work.
#     A client needs the registered game for those maps, as for any dm map.
#   * THE CVARS - server.cfg in this directory (k_fb_enabled, autoadd/remove,
#     skill), and the start map moves from "start" (no waypoints, so no bots)
#     to dm4, with k_defmap dm4 so an emptied server returns there.
# KTX adds bots ONLY while a human is on the server (BotStartFrame counts
# human_count), so an idle server shows no bots - by design, not a fault.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QW="${1:-$HOME/qw-server}"
Q1="${2:-$HOME/quake1-server}"
KTX_TAG=1.47
KTX_COMMIT=ce329889f97cc5bacf85b6388d3c5d8f242769fd
CACHE="${CACHE:-$HOME/.cache/game-servers/ktx-$KTX_TAG}"

[ -d "$QW/ktx" ] || { echo "no $QW/ktx" >&2; exit 1; }
if [ ! -d "$CACHE/.git" ]; then
  git clone -q --depth 1 --branch "$KTX_TAG" https://github.com/QW-Group/ktx.git "$CACHE"
fi
[ "$(git -C "$CACHE" rev-parse HEAD)" = "$KTX_COMMIT" ] \
  || { echo "KTX $KTX_TAG is not $KTX_COMMIT - refusing" >&2; exit 1; }

echo "== waypoints -> $QW/ktx/bots/maps"
mkdir -p "$QW/ktx/bots/maps"
cp -p "$CACHE/resources/example-configs/ktx/bots/maps/"*.bot "$QW/ktx/bots/maps/"
echo "   $(ls "$QW/ktx/bots/maps" | wc -l) map(s)"

if [ ! -f "$QW/id1/pak1.pak" ]; then
  [ -f "$Q1/id1/pak1.pak" ] || { echo "no $Q1/id1/pak1.pak to copy" >&2; exit 1; }
  echo "== pak1.pak (registered) from $Q1/id1"
  cp -p "$Q1/id1/pak1.pak" "$QW/id1/pak1.pak"
fi

echo "== server.cfg"
[ -f "$QW/id1/server.cfg.pre-bots" ] || cp -p "$QW/id1/server.cfg" "$QW/id1/server.cfg.pre-bots"
install -m 644 "$HERE/server.cfg" "$QW/id1/server.cfg"

UNIT="$HOME/.config/systemd/user/quakeworld-server.service"
if [ -f "$UNIT" ] && grep -q '+map start' "$UNIT"; then
  sed -i 's/+map start/+map dm4/' "$UNIT"
  systemctl --user daemon-reload
fi
echo "== done. systemctl --user restart quakeworld-server"
