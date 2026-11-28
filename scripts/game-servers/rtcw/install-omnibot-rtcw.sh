#!/usr/bin/env bash
# Install Omni-Bot into the fleet's RTCW dedicated server - the ONLY way this
# game can have bots.
#
# WHY NOT THE ENGINE'S OWN BOTS.  RTCW multiplayer shipped without bots in
# 2001.  The MP qagame still carries Quake III's bot scaffolding, so `addbot`
# looks like it should work, and it cannot:
#
#     rcon addbot sarge 3   ->  Error: Bot 'sarge' not defined
#
#   * no bot DEFINITIONS - not one of the 15 pk3s in main/ has a botfiles/
#     directory, so there is no "sarge" to add;
#   * no NAVIGATION - the engine needs .aas files to walk a map.  pak0.pk3 has
#     64 of them and every one is for a SINGLE-PLAYER map (castle, chateau,
#     crypt...).  `maps/mp_*.aas` across all 15 paks: zero.
#   * the engine never even sets its botlib up ("SV_BotLibSetup without
#     SV_BotInitBotLib"), so this is not something a live cvar can switch on.
#
# Omni-Bot brings its OWN waypoint navigation (309 nav files, covering all 14
# of our rotation maps) and its OWN qagame, loaded as the mod `omnibot`.
#
# THE TRAP THAT DECIDES WHICH RELEASE YOU WANT: Omni-Bot 0.86 and 0.87 ship
# only i386 Linux binaries (omnibot_rtcw.so, qagame.mp.i386.so), which our
# x86_64 iowolfded cannot load - that route needs a 32-bit engine build and
# 32-bit dev libs.  0.9 and later ship qagame.mp.x86_64.so AND
# omnibot_rtcw.x86_64.so, so the existing 64-bit engine runs them unchanged.
# Use 0.93 or newer.  (0.87 also had a separate _64bit zip; 0.9+ folded every
# arch into the one archive.)
#
# Usage:  install-omnibot-rtcw.sh [server-dir] [version]
set -euo pipefail

SRV="${1:-$HOME/rtcw-server}"
VER="${2:-0.93}"
URL="https://github.com/jswigart/omni-bot/releases/download/${VER}/omni-bot_0_${VER//./}_RTCW.zip"
UNIT="$HOME/.config/systemd/user/rtcw-server.service"
MINBOTS="${MINBOTS:-8}"        # bots on the server (both teams, auto-balanced)
DIFFICULTY="${DIFFICULTY:-1}"  # 1 easiest .. 6 hardest. The README's "0 to 5,
                               # default 3" is STALE - rtcw_commands.gm clamps -1..7
                               # (-1 = random per bot) and def_bot.gm documents 1..6.
MOVESKILL="${MOVESKILL:-1}"    # 0 off, 1 easy, 2 medium, 3 hard, 4 random

[ -x "$SRV/iowolfded.x86_64" ] || { echo "no iowolfded.x86_64 in $SRV" >&2; exit 1; }

# 1. fetch into its own empty directory and unpack there - never into the
#    server tree, so a bad archive cannot land half-installed on a live server.
STAGE="$(mktemp -d /tmp/omnibot-rtcw-XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT
echo ">> downloading Omni-Bot $VER"
curl -sSL -o "$STAGE/ob.zip" "$URL"
unzip -q "$STAGE/ob.zip" -d "$STAGE/x"

for f in x/omnibot/qagame.mp.x86_64.so x/omni-bot/omnibot_rtcw.x86_64.so; do
    [ -f "$STAGE/$f" ] || { echo "FAIL: $VER has no $f - see the 32-bit trap above" >&2; exit 1; }
    file "$STAGE/$f" | grep -q 'ELF 64-bit.*x86-64' \
        || { echo "FAIL: $f is not x86-64" >&2; exit 1; }
done

# 2. install the two halves: the MOD (game dir, qagame + client paks) and the
#    BOT (library + waypoints + gm scripts).
echo ">> installing into $SRV"
mkdir -p "$SRV/omnibot" "$SRV/omni-bot"
cp -a "$STAGE/x/omnibot/." "$SRV/omnibot/"
cp -a "$STAGE/x/omni-bot/." "$SRV/omni-bot/"
# keep only what this engine loads - another arch's qagame in the mod dir is
# dead weight the engine will try before the right one on a mixed build.
rm -f "$SRV"/omnibot/qagame.mp.i386.so "$SRV"/omnibot/qagame.mp.aarch64.so \
      "$SRV"/omnibot/qagame_mp_x86.dll "$SRV"/omnibot/qagame_mp_x64.dll \
      "$SRV"/omnibot/qagame.mp.x86_64.dylib
rm -rf "$SRV/omnibot/with_lua"
rm -f "$SRV"/omni-bot/omnibot_rtcw.so "$SRV"/omni-bot/omnibot_rtcw.aarch64.so \
      "$SRV"/omni-bot/omnibot_rtcw.dll "$SRV"/omni-bot/omnibot_rtcw_x64.dll \
      "$SRV"/omni-bot/omnibot_rtcw_mac.so
mkdir -p "$SRV/omni-bot/logs" "$SRV/omni-bot/rtcw/user"

# 3. bot count and skill.  The bot library reads this file and rewrites it on
#    exit, so it is the persistent home for both - the `bot` rcon commands
#    below change the SAME values live.
cat > "$SRV/omni-bot/rtcw/user/omni-bot.cfg" <<CFG
[ServerManager]
MinBots = $MINBOTS
MaxBots = $MINBOTS
[Difficulty]
CurrentDifficulty = $DIFFICULTY
[CombatMovement]
moveskill = $MOVESKILL
CFG

# 3b. THE BOT PROFILE - this is what stops them reading as aimbots.
#     Turning the difficulty down does NOT fix "it feels like they are
#     cheating", because the stock profile is the part that feels unfair:
#     a bot stays aimed at you for 2 s AFTER you break line of sight (from
#     the receiving end that is being tracked through a wall), sees 4500
#     units, and turns at 720 deg/s - a 180 flick in a quarter second.
#     def_bot.gm is read when a bot JOINS, so `bot kickall` (they refill to
#     MinBots) applies a change with no server restart.
PROFILE="$SRV/omni-bot/rtcw/scripts/def_bot.gm"
[ -f "$PROFILE.stock" ] || cp -a "$PROFILE" "$PROFILE.stock"
python3 - "$PROFILE" <<'PROF'
import sys
p = sys.argv[1]
s = open(p).read()
for old, new in [("this.MemorySpan = 2.0;",       "this.MemorySpan = 1.0;"),
                 ("this.AimPersistance = 2.0;",   "this.AimPersistance = 0.5;"),
                 ("this.MaxViewDistance = 4500;", "this.MaxViewDistance = 2500;"),
                 ("this.MaxTurnSpeed = 720.0;",   "this.MaxTurnSpeed = 300.0;"),
                 ("this.AimStiffness = 75.0;",    "this.AimStiffness = 40.0;")]:
    if old in s:                      # applied to stock text, so re-running is a no-op
        s = s.replace(old, new + "\t// NSC fleet (stock " + old.split("= ")[1] + ")", 1)
open(p, "w").write(s)
PROF
grep -q "AimPersistance = 0.5" "$PROFILE" || { echo "FAIL: bot profile not tuned" >&2; exit 1; }
# AimDamping is deliberately LEFT at 10: lowering stiffness 75 -> 40 moves the
# aim spring CLOSER to critical damping (2*sqrt(40)=12.6 vs 2*sqrt(75)=17.3),
# so it damps the oscillation def_bot.gm warns about rather than causing it.

# 4. server.cfg.  omnibot_enable/omnibot_path MUST be set BEFORE the map
#    loads, so they go above the `vstr d1` that starts the rotation.
CFG_FILE="$SRV/main/server.cfg"
if ! grep -q omnibot_enable "$CFG_FILE"; then
    cp -a "$CFG_FILE" "$CFG_FILE.pre-omnibot"
    python3 - "$CFG_FILE" "$SRV" <<'PY'
import sys
path, srv = sys.argv[1], sys.argv[2]
s = open(path).read()
block = f"""
// ---------------------------------------------------------------- Omni-Bot
// RTCW MP shipped with no bots at all: the MP paks carry no bot definitions
// and no mp_*.aas navigation, so the engine's `addbot` can never work.  Bots
// here are Omni-Bot, which brings its own waypoint navigation and its own
// qagame, loaded as the mod `omnibot` (set on the unit's ExecStart).
// Counts/skill live in omni-bot/rtcw/user/omni-bot.cfg, settable live:
//   rcon bot minbots 8      (bots on the server, balanced over both teams)
//   rcon bot difficulty 2   (0 easiest .. 5 hardest, default 3)
//   rcon bot moveskill 2    (0 off, 1 easy, 2 medium, 3 hard, 4 random)
set omnibot_enable 1
set omnibot_path "{srv}/omni-bot"

// start the rotation (LAST - omnibot_* must be set before the map loads)
"""
marker = "vstr d1"
i = s.rstrip().rfind(marker)
assert i != -1, "server.cfg does not end by starting the rotation with vstr d1"
s = s[:i].rstrip("\n") + "\n" + block + "vstr d1\n"
open(path, "w").write(s)
PY
fi

# 5. the unit loads the mod.
if [ -f "$UNIT" ] && ! grep -q 'fs_game omnibot' "$UNIT"; then
    cp -a "$UNIT" "$UNIT.pre-omnibot"
    sed -i 's|+set dedicated 2|+set fs_game omnibot +set dedicated 2|' "$UNIT"
    systemctl --user daemon-reload
fi

echo ">> restarting rtcw-server"
systemctl --user restart rtcw-server
sleep 25

# 6. VERIFY THE POST-CONDITION, not the exit code.  "the unit is active" was
#    true for the whole time this server had no bots.
LOG="$SRV/server.log"
grep -q 'Omni-bot .* initialized' "$LOG" \
    || { echo "FAIL: the bot library never initialized - see $LOG" >&2; exit 1; }
BOTS=$(grep -ac 'entered the game' "$LOG" || true)
echo ">> $(grep -a 'Omni-bot .* initialized' "$LOG" | tail -1)"
echo ">> bots that entered the game: $BOTS"
[ "$BOTS" -gt 0 ] || { echo "FAIL: library loaded but no bot joined" >&2; exit 1; }

cat <<MSG

OK. Bots are on rtcw-server.

  live:   rcon bot minbots N | bot difficulty 0..5 | bot moveskill 0..4
  file:   $SRV/omni-bot/rtcw/user/omni-bot.cfg
  undo:   rm -rf $SRV/omnibot $SRV/omni-bot
          mv $CFG_FILE.pre-omnibot $CFG_FILE
          mv $UNIT.pre-omnibot $UNIT && systemctl --user daemon-reload
          systemctl --user restart rtcw-server

CLIENTS: the server now runs the mod \`omnibot\`, so a joining client needs
omnibot/omnibot_rtcw.pk3 + omnibot/ob_media.pk3 in its RTCW folder.  They are
staged in the library at Games-Library/ReturnToCastleWolfenstein/omnibot/, and
a client WITHOUT them still joins - it downloads them in-game over UDP
(sv_allowDownload 1), which just takes a couple of minutes.
MSG
