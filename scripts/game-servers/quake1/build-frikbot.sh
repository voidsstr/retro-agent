#!/usr/bin/env bash
# Build the quake1-server's progs.dat: id's own QuakeC (v101qc, GPL, from
# id-Software/Quake-Tools) + FrikBot X 0.10.2 (public domain) + fleet.qc, and
# install it as the server-only gamedir "fbx".
#
#   build-frikbot.sh [q1dir]     (default ~/quake1-server)
#
# SERVER-SIDE ONLY. NetQuake never tells a client which gamedir the server
# runs, and these progs precache nothing a retail id1 install lacks (FrikBot's
# only additions are progs/s_light.spr and progs/s_bubble.spr, both in pak0),
# so the staged GLQUAKE.EXE / WINQUAKE.EXE join with nothing to download.
# A loose progs.dat in id1/ would NOT work: Quake searches a directory's paks
# before its loose files, so pak0's progs.dat would win. Hence -game fbx.
#
# Pinned sources (all fetched over https, nothing vendored but fleet.qc):
#   id-Software/Quake-Tools  qcc/v101qc         c0d1b91c74eb654365ac7755bc837e497caaca73
#   teknoskillz/FrikBot-X-0.10.2--Revision-B-  7da759cb2cd051b8a2b8777494f56de564a0ca4b (FrikaC's 0.10.2 files)
#   graphitemaster/gmqcc     the QuakeC compiler b2a319efb41069b9a250e76991dbe505ff8d030d
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
Q1="${1:-$HOME/quake1-server}"
CACHE="${CACHE:-$HOME/.cache/game-servers/frikbot}"
IDQT_COMMIT=c0d1b91c74eb654365ac7755bc837e497caaca73
FBX_COMMIT="${FBX_COMMIT:-7da759cb2cd051b8a2b8777494f56de564a0ca4b}"
GMQCC_COMMIT=b2a319efb41069b9a250e76991dbe505ff8d030d

fetch() {  # repo dir commit
  if [ ! -d "$2/.git" ]; then git clone -q "$1" "$2"; fi
  git -C "$2" fetch -q origin
  [ -n "$3" ] && git -C "$2" checkout -q -f "$3"
  return 0
}
mkdir -p "$CACHE"
fetch https://github.com/id-Software/Quake-Tools.git "$CACHE/Quake-Tools" "$IDQT_COMMIT"
fetch https://github.com/teknoskillz/FrikBot-X-0.10.2--Revision-B-.git "$CACHE/fbx" "$FBX_COMMIT"
fetch https://github.com/graphitemaster/gmqcc.git "$CACHE/gmqcc" "$GMQCC_COMMIT"
[ -x "$CACHE/gmqcc/gmqcc" ] || make -C "$CACHE/gmqcc" -j"$(nproc)" gmqcc >/dev/null

SRC="$CACHE/src"
rm -rf "$SRC"; mkdir -p "$SRC/frikbot/waypoints"
cp "$CACHE/Quake-Tools/qcc/v101qc/"*.qc "$CACHE/Quake-Tools/qcc/v101qc/progs.src" "$SRC/"
cp "$CACHE/fbx/"bot*.qc "$SRC/frikbot/"
cp "$CACHE/fbx/waypoints/"*.qc "$SRC/frikbot/waypoints/"
cp "$HERE/fleet.qc" "$SRC/"

# FrikBot's install.txt, step by step - done by a script so it is the same
# every time. A step that does not apply aborts the build.
python3 - "$SRC" <<'PYEOF'
import re, sys, os
src = sys.argv[1]
def rd(n): return open(os.path.join(src, n), encoding="latin-1").read()
def wr(n, s): open(os.path.join(src, n), "w", encoding="latin-1").write(s)

# 1. defs.qc: FrikBot redefines these builtins with bot-aware wrappers.
defs = rd("defs.qc")
for name in ("sound", "stuffcmd", "sprint", "aim", "centerprint", "setspawnparms",
             "WriteByte", "WriteChar", "WriteShort", "WriteLong", "WriteCoord",
             "WriteAngle", "WriteString", "WriteEntity"):
    # [ \t] not \s: \s crosses newlines and once swallowed a blank line
    # instead of the builtin.
    pat = re.compile(r"(?m)^([ \t]*[a-z]+[ \t]*\([^)\n]*\)[ \t]*" + name + r"[ \t]*=[ \t]*#\d+[ \t]*;.*)$")
    defs, n = pat.subn(r"// FrikBot: \1", defs)
    assert n == 1, ("defs.qc builtin", name, n)
wr("defs.qc", defs)

def insert_top(fname, func, line):
    s = rd(fname)
    m = re.search(r"(?m)^void\(\)\s*" + func + r"\s*=\s*\n\{[^\n]*\n", s)
    assert m, (fname, func)
    s = s[:m.end()] + "\t" + line + "\n" + s[m.end():]
    wr(fname, s)

# 2. world.qc: BotInit before InitBodyQue in worldspawn, BotFrame first in StartFrame.
w = rd("world.qc")
m = re.search(r"(?ms)^void\(\) worldspawn =.*?^\s*InitBodyQue \(\);", w)
assert m
i = w.rfind("InitBodyQue", m.start(), m.end())
w = w[:i] + "BotInit();\t// FrikBot\n\t" + w[i:]
wr("world.qc", w)
insert_top("world.qc", "StartFrame", "BotFrame();\t// FrikBot\n\tFleetBotFrame();\t// NSC fleet backfill (fleet.qc)")

# 3. client.qc hooks.
insert_top("client.qc", "PlayerPreThink", "if (BotPreFrame())\t// FrikBot\n\t\treturn;")
insert_top("client.qc", "PlayerPostThink", "if (BotPostFrame())\t// FrikBot\n\t\treturn;")
insert_top("client.qc", "ClientConnect", "ClientInRankings();\t// FrikBot")
insert_top("client.qc", "ClientDisconnect", "ClientDisconnected();\t// FrikBot")

# 4. progs.src: output name, then the bot files right after defs.qc.
p = rd("progs.src").replace("\r\n", "\n")
lines = p.split("\n")
assert lines[0].strip() == "../progs.dat"
lines[0] = "progs.dat"
at = lines.index("defs.qc") + 1
add = ["frikbot/waypoints/map_dm%d.qc" % i for i in range(1, 7)] + [
    "frikbot/bot.qc", "frikbot/bot_way.qc", "frikbot/bot_fight.qc",
    "frikbot/bot_ai.qc", "frikbot/bot_misc.qc", "frikbot/bot_phys.qc",
    "frikbot/bot_move.qc", "frikbot/bot_ed.qc", "fleet.qc"]
lines[at:at] = add
wr("progs.src", "\n".join(lines))
PYEOF

( cd "$SRC" && "$CACHE/gmqcc/gmqcc" -std=qcc -O2 ) > "$CACHE/build.log" 2>&1 \
  || { tail -30 "$CACHE/build.log"; echo "QuakeC BUILD FAILED" >&2; exit 1; }
test -s "$SRC/progs.dat"
mkdir -p "$Q1/fbx"
install -m 644 "$SRC/progs.dat" "$Q1/fbx/progs.dat"
echo "== progs.dat -> $Q1/fbx ($(stat -c %s "$Q1/fbx/progs.dat") bytes, $(md5sum < "$Q1/fbx/progs.dat" | cut -c1-12))"
echo "== the unit must run with -game fbx (quake1-server.service)"
