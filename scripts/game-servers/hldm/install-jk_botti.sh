#!/usr/bin/env bash
# Install jk_botti (Half-Life Deathmatch bots) into the hldm-server HLDS tree.
#
#   install-jk_botti.sh [tree] [max_bots] [skill]
#
#   tree      the HLDS dir holding valve/   (default ~/hldm-server)
#   max_bots  backfill target - bots fill the server up to this many PLAYERS,
#             and each human who joins kicks one (default 3, the fleet rule)
#   skill     jk_botti skill 1..5, where 1 is the BEST and 5 the worst
#             (default 2 = the fleet's "Hard": 4th of 5 from the easiest,
#             the same rung as YaPB 3 and Quake III's Hardcore)
#
# Server-side only. jk_botti bots are fake clients of the stock `valve` game,
# so a vanilla client (the CounterStrike16 tree's hl.exe -game valve, which is
# what joins this server - see README.md) needs nothing new.
#
# Payload (committed beside this script, both GPL, from the Bots-United GitHub
# releases):
#   bots/metamod-p-v1.21p109-linux_ubuntu1804.tar.xz   - the same metamod-p
#        build cs16-noblood already runs on this host's HLDS
#   bots/jk_botti-v1.62-linux_ubuntu1804.tar.xz        - ships waypoints for
#        every map in cfg/mapcycle.txt (and autowaypoints anything else)
#
# Idempotent: re-running rewrites the fleet block of jk_botti.cfg and leaves the
# rest alone. Restart the unit afterwards: systemctl --user restart hldm-server
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE="${1:-$HOME/hldm-server}"
MAXBOTS="${2:-3}"
SKILL="${3:-2}"
MM_TAR="$HERE/bots/metamod-p-v1.21p109-linux_ubuntu1804.tar.xz"
JK_TAR="$HERE/bots/jk_botti-v1.62-linux_ubuntu1804.tar.xz"
MM_URL="https://github.com/Bots-United/metamod-p/releases/download/v1.21p109/metamod-p-v1.21p109-linux_ubuntu1804.tar.xz"
JK_URL="https://github.com/Bots-United/jk_botti/releases/download/v1.62/jk_botti-v1.62-linux_ubuntu1804.tar.xz"

GAMEDIR="$TREE/valve"
[ -d "$GAMEDIR" ] || { echo "no $GAMEDIR" >&2; exit 1; }
case "$SKILL" in [1-5]) ;; *) echo "skill must be 1..5 (1 best)" >&2; exit 1;; esac

mkdir -p "$HERE/bots"
[ -f "$MM_TAR" ] || curl -fsSL -o "$MM_TAR" "$MM_URL"
[ -f "$JK_TAR" ] || curl -fsSL -o "$JK_TAR" "$JK_URL"

echo "== unpacking metamod-p + jk_botti into $GAMEDIR/addons"
# Keep an existing jk_botti.cfg: the tarball would overwrite it, and the fleet
# block below is rewritten anyway.
CFG="$GAMEDIR/addons/jk_botti/jk_botti.cfg"
[ -f "$CFG" ] && cp -p "$CFG" "$CFG.keep"
tar xJf "$MM_TAR" -C "$GAMEDIR"
tar xJf "$JK_TAR" -C "$GAMEDIR"
[ -f "$CFG.keep" ] && mv -f "$CFG.keep" "$CFG"
test -f "$GAMEDIR/addons/metamod/dlls/metamod.so"
test -f "$GAMEDIR/addons/jk_botti/dlls/jk_botti_mm.so"
# jk_botti writes its compiled waypoint matrices next to the .wpt files.
chmod -R u+rwX "$GAMEDIR/addons/jk_botti"

# One stock name is a slur ("[AG]FragFag"); drop it so a bot never wears it.
sed -i '/FragFag/d' "$GAMEDIR/addons/jk_botti/jk_botti_names.txt"

PI="$GAMEDIR/addons/metamod/plugins.ini"
touch "$PI"
grep -q 'jk_botti/dlls/jk_botti_mm.so' "$PI" || echo 'linux addons/jk_botti/dlls/jk_botti_mm.so' >> "$PI"

# The gamedll slot goes to metamod, which loads dlls/hl.so itself (its built-in
# table maps gamedir "valve" to it). The original liblist.gam is kept.
LL="$GAMEDIR/liblist.gam"
[ -f "$LL.pre-bots" ] || cp -p "$LL" "$LL.pre-bots"
if grep -q '^gamedll_linux' "$LL"; then
  sed -i 's#^gamedll_linux .*#gamedll_linux "addons/metamod/dlls/metamod.so"#' "$LL"
else
  echo 'gamedll_linux "addons/metamod/dlls/metamod.so"' >> "$LL"
fi

# The stock cfg adds five FIXED bots of skills 1-5 ("addbot" lines). Those are
# replaced by the fleet's backfill: min_bots 0 / max_bots N means bots fill the
# server to N players and each joining human kicks one.
python3 - "$CFG" "$MAXBOTS" "$SKILL" <<'EOF'
import re, sys
path, maxbots, skill = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(path, encoding="latin-1").read()
mark = "# ---- NSC fleet bots"
text = text.split(mark)[0].rstrip("\n") + "\n"
# Comment out the stock fixed addbots and default botskill; the block below
# is the only place the fleet sets them.
text = re.sub(r"(?m)^(addbot\b.*|botskill\b.*|min_bots\b.*|max_bots\b.*)$",
              r"#\1  # disabled by install-jk_botti.sh", text)
text += f"""
{mark} (install-jk_botti.sh; re-run it rather than editing here)
# Backfill to {maxbots} players: an empty server shows {maxbots} bots and each
# human who joins replaces one. Skill is 1 (best) .. 5 (worst); {skill} = "Hard".
botskill {skill}
min_bots 0
max_bots {maxbots}
"""
open(path, "w", encoding="latin-1").write(text)
EOF
grep -n -A6 'NSC fleet bots' "$CFG"
echo "== done. systemctl --user restart hldm-server ; then 'meta list' via rcon"
