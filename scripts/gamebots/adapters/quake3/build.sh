#!/usr/bin/env bash
# Build qagame_ai.so — ioquake3's game module with the gamebots adapter in it.
#
# Builds the module DIRECTLY rather than through ioq3's CMake, because we need
# exactly one target (the native x86-64 baseq3 game .so) and CMake would build
# the cgame, the UI and the QVMs too. The source list is copied from
# cmake/basegame.cmake's GAME_SOURCES and is checked against it at build time,
# so an upstream file addition fails the build instead of silently linking a
# module missing a translation unit.
#
#   ./build.sh              # build
#   ./build.sh --clean
#
# The result is a drop-in replacement for /usr/lib/ioquake3/baseq3/qagame.so.
# It is INERT until `gb_enable 1` — installing it changes nothing by itself.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOQ3="$HERE/build/ioq3"
GAME="$IOQ3/code/game"
OUT="$HERE/out"
GBDIR="$(cd "$HERE/../.." && pwd)"          # scripts/gamebots
CC="${CC:-gcc}"

[ "${1:-}" = "--clean" ] && { rm -rf "$OUT"; echo "cleaned"; exit 0; }

if [ ! -d "$GAME" ]; then
    echo "ioq3 source not found at $IOQ3" >&2
    echo "  git clone --depth 1 https://github.com/ioquake/ioq3.git $IOQ3" >&2
    exit 1
fi

# Regenerate the header from the schema, so a schema change can never leave the
# adapter compiled against a stale layout. The runtime hash would catch it, but
# at the first tick of a live server rather than here.
python3 "$GBDIR/schema.py" --emit-header > "$GBDIR/gamebots_schema.h"

SRCS="g_main.c ai_chat.c ai_cmd.c ai_dmnet.c ai_dmq3.c ai_main.c ai_team.c
      ai_vcmd.c bg_misc.c bg_pmove.c bg_slidemove.c bg_lib.c g_active.c
      g_arenas.c g_bot.c g_client.c g_cmds.c g_combat.c g_items.c g_mem.c
      g_misc.c g_missile.c g_mover.c g_session.c g_spawn.c g_svcmds.c
      g_target.c g_team.c g_trigger.c g_utils.c g_weapon.c g_syscalls.c"

# q_math.c and q_shared.c are NOT in basegame.cmake's GAME_SOURCES -- they come
# in through the binary-module target separately. Leaving them out builds a .so
# that links fine and then fails at dlopen with "undefined symbol: vec3_origin",
# which reads like a corrupt module rather than a missing translation unit, and
# the engine quietly falls back to the QVM so the bots look normal.
QCOMMON_SRCS="q_math.c q_shared.c"
# Collapse the newlines: the drift check below matches " $f " and a
# file sitting next to a line break would not match its own entry.
SRCS="$(echo $SRCS)"

# Guard against upstream drift: if basegame.cmake lists a game source we do not,
# we would link a module quietly missing that file's symbols.
CM="$IOQ3/cmake/basegame.cmake"
if [ -f "$CM" ]; then
    missing=""
    for f in $(sed -n '/^set(GAME_SOURCES/,/^)/p' "$CM" |
               grep -oE 'game/[a-z_0-9]+\.c' | sed 's|game/||'); do
        case " $SRCS " in *" $f "*) ;; *) missing="$missing $f" ;; esac
    done
    if [ -n "$missing" ]; then
        echo "ioq3 has game sources this script does not build:$missing" >&2
        echo "add them to SRCS above" >&2
        exit 1
    fi
fi

# Apply our hooks to ai_main.c. Idempotent: a fresh clone gets patched, an
# already-patched tree is left alone. Keeping this as a patch rather than a
# forked copy of ai_main.c means an ioq3 update shows up as a patch conflict
# instead of silently reverting our hooks.
if ! grep -q GB_FrameBegin "$GAME/ai_main.c"; then
    echo "applying ai_main.patch"
    patch -s -p0 -d "$GAME" < "$HERE/ai_main.patch" || {
        echo "ai_main.patch did not apply — ioq3 has moved; re-make the patch" >&2
        exit 1
    }
fi

mkdir -p "$OUT"
echo "building qagame_ai.so ($(cd "$IOQ3" && git rev-parse --short HEAD 2>/dev/null || echo unknown))"

OBJS=""
for f in $SRCS; do
    o="$OUT/${f%.c}.o"
    $CC -c -O2 -fPIC -fno-strict-aliasing -w \
        -I"$GAME" -I"$IOQ3/code/qcommon" -I"$HERE" -I"$GBDIR" -I"$GBDIR/adapters" \
        "$GAME/$f" -o "$o"
    OBJS="$OBJS $o"
done
for f in $QCOMMON_SRCS; do
    o="$OUT/${f%.c}.o"
    $CC -c -O2 -fPIC -fno-strict-aliasing -w \
        -I"$GAME" -I"$IOQ3/code/qcommon" \
        "$IOQ3/code/qcommon/$f" -o "$o"
    OBJS="$OBJS $o"
done

# Ours, built with warnings ON — we are not obliged to be quiet about a 1999
# codebase, but we are about our own code.
for f in "$HERE/gb_adapter.c" "$GBDIR/adapters/gb_client.c"; do
    o="$OUT/$(basename "${f%.c}").o"
    $CC -c -O2 -fPIC -Wall -Wextra -Wno-unused-parameter \
        -I"$GAME" -I"$IOQ3/code/qcommon" -I"$GBDIR" -I"$GBDIR/adapters" \
        "$f" -o "$o"
    OBJS="$OBJS $o"
done

$CC -shared -o "$OUT/qagame_ai.so" $OBJS -lm
echo "built $OUT/qagame_ai.so ($(stat -c%s "$OUT/qagame_ai.so") bytes)"

# The engine dlopen()s this and calls vmMain; if that symbol is missing the
# server loads and then fails at the first frame with a message that does not
# name the cause.
if ! nm -D --defined-only "$OUT/qagame_ai.so" | grep -q ' T vmMain'; then
    echo "ERROR: vmMain is not exported — the engine cannot use this" >&2
    exit 1
fi
# dlopen it HERE rather than discovering an unresolved symbol when a game
# server loads it -- the engine falls back to the QVM on failure, so the bots
# keep working and nothing tells you the module was ignored.
if ! python3 -c "import ctypes,sys; ctypes.CDLL(sys.argv[1])" "$OUT/qagame_ai.so"; then
    echo "ERROR: the module does not dlopen — the engine would silently fall back" >&2
    exit 1
fi
echo "dlopen ok — all symbols resolve"
echo "vmMain exported — module is loadable"
