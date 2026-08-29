#!/usr/bin/env bash
# Build game_ai.so -- Yamagi Quake II's baseq2 game module with the gamebots
# adapter compiled in.
#
# Builds the module DIRECTLY rather than through yquake2's own Makefile,
# because we need exactly one target (the native x86-64 baseq2 game.so) and
# the real Makefile would also build the client, server and renderers. The
# source list is copied from the upstream Makefile's GAME_OBJS_ variable and
# is checked against it at build time, so an upstream file addition fails the
# build instead of silently linking a module missing a translation unit --
# the exact failure mode that bit the Quake III adapter.
#
#   ./build.sh              # build
#   ./build.sh --clean
#
# The result is a drop-in replacement for
# /usr/lib/yamagi-quake2/baseq2/game.so. It is INERT until `gb_bots` and
# `gb_enable` are both set -- installing it changes nothing by itself.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
YQ2="$HERE/build/yquake2"
GAME="$YQ2/src/game"
OUT="$HERE/out"
GBDIR="$(cd "$HERE/../.." && pwd)"          # scripts/gamebots
CC="${CC:-gcc}"
TAG="QUAKE2_8_60"                            # matches yamagi-quake2-core 8.60+dfsg-1

[ "${1:-}" = "--clean" ] && { rm -rf "$OUT"; echo "cleaned"; exit 0; }

if [ ! -d "$GAME" ]; then
    echo "yquake2 source not found at $YQ2" >&2
    echo "  git clone --depth 1 --branch $TAG https://github.com/yquake2/yquake2.git $YQ2" >&2
    exit 1
fi

# Regenerate the header from the schema, so a schema change can never leave
# the adapter compiled against a stale layout. The runtime hash would catch
# it too, but at the first tick of a live server rather than here.
python3 "$GBDIR/schema.py" --emit-header > "$GBDIR/gamebots_schema.h"

# The whole GAME_OBJS_ list from yquake2's top-level Makefile: everything
# baseq2/game.so is linked from, minus the .o suffix. g_ai.c has nothing to
# do with our AI -- it is the vanilla MONSTER ai (patrol/hunt/dodge) that
# ships in stock Quake II and has to be linked in regardless.
SRCS="g_ai.c g_chase.c g_cmds.c g_combat.c g_func.c g_items.c g_main.c
      g_misc.c g_monster.c g_phys.c g_spawn.c g_svcmds.c g_target.c
      g_trigger.c g_turret.c g_utils.c g_weapon.c
      monster/berserker/berserker.c monster/boss2/boss2.c
      monster/boss3/boss3.c monster/boss3/boss31.c monster/boss3/boss32.c
      monster/brain/brain.c monster/chick/chick.c monster/flipper/flipper.c
      monster/float/float.c monster/flyer/flyer.c
      monster/gladiator/gladiator.c monster/gunner/gunner.c
      monster/hover/hover.c monster/infantry/infantry.c
      monster/insane/insane.c monster/medic/medic.c monster/misc/move.c
      monster/mutant/mutant.c monster/parasite/parasite.c
      monster/soldier/soldier.c monster/supertank/supertank.c
      monster/tank/tank.c
      player/client.c player/hud.c player/trail.c player/view.c
      player/weapon.c
      savegame/savegame.c"
# Collapse newlines so the drift check's " $f " substring match cannot be
# defeated by a file sitting next to a line break.
SRCS="$(echo $SRCS)"

# common/shared/*.c is compiled into game.so too (GAME_OBJS_ lists these
# explicitly) -- it is NOT pulled in by including header/local.h, only by
# linking the .o. Omit these and the module links fine, then fails dlopen
# with "undefined symbol", exactly the q_math.c/q_shared.c trap the Quake
# III build script documents.
COMMON_SRCS="common/shared/flash.c common/shared/rand.c common/shared/shared.c"

if [ -f "$YQ2/Makefile" ]; then
    missing=""
    block="$(sed -n '/^GAME_OBJS_ = \\/,/^$/p' "$YQ2/Makefile")"
    for f in $(echo "$block" | grep -oE 'src/game/[a-zA-Z0-9_/]+\.o' | sed -e 's|^src/game/||' -e 's|\.o$|.c|'); do
        case " $SRCS " in *" $f "*) ;; *) missing="$missing $f" ;; esac
    done
    for f in $(echo "$block" | grep -oE 'src/common/shared/[a-zA-Z0-9_]+\.o' | sed -e 's|^src/||' -e 's|\.o$|.c|'); do
        case " $COMMON_SRCS " in *" $f "*) ;; *) missing="$missing $f" ;; esac
    done
    if [ -n "$missing" ]; then
        echo "yquake2 GAME_OBJS_ lists sources this script does not build:$missing" >&2
        echo "add them to SRCS/COMMON_SRCS above" >&2
        exit 1
    fi
fi

# Apply our hook to g_main.c. Idempotent: a fresh clone gets patched, an
# already-patched tree is left alone. Keeping this as a patch rather than a
# forked copy of g_main.c means a yquake2 update shows up as a patch
# conflict instead of silently reverting our hooks.
if ! grep -q GB_RunFrame "$GAME/g_main.c"; then
    echo "applying g_main.patch"
    patch -s -p0 -d "$GAME" < "$HERE/g_main.patch" || {
        echo "g_main.patch did not apply -- yquake2 has moved; re-make the patch" >&2
        exit 1
    }
fi

mkdir -p "$OUT"
echo "building game_ai.so ($(cd "$YQ2" && git rev-parse --short HEAD 2>/dev/null || echo unknown))"

# Same flags the upstream Makefile uses for release/baseq2/game.so, including
# the two -D's the Makefile normally computes from `uname` (savegame.c's
# cross-platform-savegame check #errors out without them).
YQ2_OSTYPE="$(uname -s)"
YQ2_ARCH="$(uname -m | sed -e 's/i.86/i386/' -e 's/amd64/x86_64/' \
                           -e 's/arm64/aarch64/' -e 's/^arm.*/arm/')"
CFLAGS="-O2 -Wall -pipe -fomit-frame-pointer -fno-strict-aliasing -fwrapv \
        -fvisibility=hidden -fPIC -Wno-unused-result -w \
        -DYQ2OSTYPE=\"$YQ2_OSTYPE\" -DYQ2ARCH=\"$YQ2_ARCH\""

OBJS=""
for f in $SRCS; do
    o="$OUT/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')"
    $CC -c $CFLAGS -I"$GAME" -I"$YQ2/src/common" -I"$HERE" -I"$GBDIR" -I"$GBDIR/adapters" \
        "$GAME/$f" -o "$o"
    OBJS="$OBJS $o"
done
for f in $COMMON_SRCS; do
    o="$OUT/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')"
    $CC -c $CFLAGS -I"$YQ2/src/game" -I"$YQ2/src/common" "$YQ2/src/$f" -o "$o"
    OBJS="$OBJS $o"
done

# Ours, built with warnings ON -- we are not obliged to be quiet about the
# 2001 baseq2 codebase, but we are about our own code.
for f in "$HERE/gb_adapter.c" "$GBDIR/adapters/gb_client.c"; do
    o="$OUT/$(basename "${f%.c}").o"
    $CC -c -O2 -fPIC -Wall -Wextra -Wno-unused-parameter \
        -I"$GAME" -I"$YQ2/src/common" -I"$HERE" -I"$GBDIR" -I"$GBDIR/adapters" \
        "$f" -o "$o"
    OBJS="$OBJS $o"
done

$CC -shared -o "$OUT/game_ai.so" $OBJS -lm
echo "built $OUT/game_ai.so ($(stat -c%s "$OUT/game_ai.so") bytes)"

# The engine dlopen()s this and calls GetGameAPI; if that symbol is missing
# or hidden the server loads the module and then fails at the first frame
# with a message that does not name the cause.
if ! nm -D --defined-only "$OUT/game_ai.so" | grep -q ' T GetGameAPI'; then
    echo "ERROR: GetGameAPI is not exported -- the engine cannot use this" >&2
    exit 1
fi
# dlopen it HERE rather than discovering an unresolved symbol when a game
# server loads it. Unlike ioquake3, Yamagi's dedicated server does not fall
# back to anything if game.so fails to load -- it just refuses to start the
# map -- but a build that "succeeds" and then can't be dlopened is still the
# worst failure mode: it looks done until someone actually points a server
# at it.
if ! python3 -c "import ctypes,sys; ctypes.CDLL(sys.argv[1])" "$OUT/game_ai.so"; then
    echo "ERROR: the module does not dlopen -- the server would refuse to load it" >&2
    exit 1
fi
echo "dlopen ok -- all symbols resolve"
echo "GetGameAPI exported -- module is loadable"

# ---------------------------------------------------------------------------
# The safety-patched engine (q2ded). NOT optional -- see README "Gotchas".
#
# Quake II fake clients never leave client_t.state == cs_free (there is no
# network handshake to advance it), and two stock server functions
# (SV_ClientPrintf in sv_send.c, PF_Unicast in sv_game.c) write into a
# cs_free client's netchan.message with no state check at all. Every item
# pickup, obituary and respawn effect targets the player who caused it via
# exactly these functions. SV_SendClientMessages() skips cs_free clients when
# flushing, so that traffic is never drained -- it queues forever and
# SZ_GetSpace() eventually calls Com_Error(ERR_FATAL), taking the whole
# server down. This is not a corner case: it was hit on a live test run
# within 20 seconds of the first bot dying. sv_fakeclient_safety.patch adds a
# one-line guard to each function. Without it, `gb_bots` above 0 WILL crash
# any Quake II server sooner or later, however careful the game-side AI is.
#
# Because the fix is server-side, not game-side, it cannot ship as part of
# game_ai.so -- Quake II's engine binary needs it too. This is built from the
# SAME clone, via yquake2's own Makefile (unlike game.so, q2ded pulls in
# enough of the engine -- filesystem, netchan, collision -- that hand-rolling
# its source list the way SRCS/COMMON_SRCS do above is not worth it).
if ! grep -q "gamebots: a fake client" "$YQ2/src/server/sv_send.c"; then
    echo "applying sv_fakeclient_safety.patch"
    patch -s -p0 -d "$YQ2" < "$HERE/sv_fakeclient_safety.patch" || {
        echo "sv_fakeclient_safety.patch did not apply -- yquake2 has moved; re-make it" >&2
        exit 1
    }
fi

mkdir -p "$YQ2/release"
( cd "$YQ2" && make release/q2ded -j"$(nproc)" WITH_CURL=no WITH_OPENAL=no >/tmp/gb_q2ded_build.log 2>&1 ) || {
    echo "ERROR: patched q2ded failed to build -- see /tmp/gb_q2ded_build.log" >&2
    exit 1
}
cp "$YQ2/release/q2ded" "$OUT/q2ded_gamebots"
echo "built $OUT/q2ded_gamebots ($(stat -c%s "$OUT/q2ded_gamebots") bytes)"

# -h exits 1 by this binary's own design (it is not an error), so check the
# banner text rather than the exit code -- and capture into a variable first
# rather than piping, so that exit-1-but-otherwise-fine doesn't trip
# `pipefail` before grep gets a look at it.
helptext="$("$OUT/q2ded_gamebots" -h 2>&1 || true)"
if ! echo "$helptext" | grep -q "Yamagi Quake II"; then
    echo "ERROR: q2ded_gamebots does not run" >&2
    exit 1
fi
echo "q2ded_gamebots runs -- use it (not the systemwide q2ded) for any server"
echo "that sets gb_bots above 0, including the live one, once someone decides"
echo "to actually deploy this."
