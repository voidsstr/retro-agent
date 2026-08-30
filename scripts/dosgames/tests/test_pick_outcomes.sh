#!/bin/bash
# test_pick_outcomes.sh - assert WHICH launcher each directory resolves to.
#
# run_dos_tests.sh's launcher checks are source greps (grep -q 'catalog_prefers'),
# which an adversarial review rightly called out: the logic can break while the
# strings survive. These cases run the real binary against fixtures rebuilt from
# the fleet Win98 box's actual directories and assert the OUTCOME.
#
# Every expectation below is a launcher the box got wrong at some point, or one
# it got right that a later fix must not disturb.
#
# Usage: bash tests/test_pick_outcomes.sh   (or via run_dos_tests.sh)
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="$(dirname "$HERE")"
WORK="${DOSGAME_WORK:-/tmp/dosgame-pick-$$}"
WATCOM="${DOSGAME_WATCOM:-$HOME/development/toolchain-dos/watcom}"
DOSBOX="${DOSGAME_DOSBOX:-dosbox}"

pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

if [ ! -x "$WATCOM/binl64/wcl" ]; then
    echo "Open Watcom not found at $WATCOM - skipping."; exit 0
fi
if ! command -v "$DOSBOX" >/dev/null 2>&1 && [ ! -x "$DOSBOX" ]; then
    echo "dosbox not found - skipping."; exit 0
fi

export WATCOM PATH="$WATCOM/binl64:$PATH" INCLUDE="$WATCOM/h"
mkdir -p "$WORK"
# Keep the work dir when the caller named one, so a failure can be inspected.
[ -n "${DOSGAME_WORK:-}" ] || trap 'rm -rf "$WORK"' EXIT

( cd "$SRCDIR" && wcl -bcl=dos -ml -os -q -wx -k8192 \
    -fe="$WORK/DOSGAME.EXE" dosgame.c ) > "$WORK/build.log" 2>&1
rm -f "$SRCDIR/dosgame.o"
[ -f "$WORK/DOSGAME.EXE" ] || { echo "BUILD FAILED"; cat "$WORK/build.log"; exit 1; }

C="$WORK/c"
mkdir -p "$C/DOSGAME"
cp "$WORK/DOSGAME.EXE" "$C/DOSGAME/DOSGAME.EXE"
cp "$SRCDIR/data/GAMES.CAT" "$C/DOSGAME/GAMES.CAT"
printf 'gamedir=C:\\GAMES\nscan=C:\\\n' > "$C/DOSGAME/DOSGAME.CFG"

mk() { d="$C/$1"; shift; mkdir -p "$d"; for f in "$@"; do : > "$d/$f"; done; }

# --- directories the box picked WRONG at some point ------------------------
mk JAGGED1  CBYTES4.COM DOXVIEW.EXE EXECUTOR.EXE GET.COM JA.DAT
mk KEENDRMS KEENDWEB.BAT KEENDR.BAT KDREAMS.CMP
mk ROTT     APOGEE.BAT 3DRCAT.EXE DEALERS.EXE ROTT.EXE ROTTIPX.EXE ROTTCOM.EXE \
            ROTTSND.EXE VENDOR.EXE DARKWAR.RTL
mk KEEN     KEEN.EXE APOGEE.BAT KEEN4E.EXE SWCBBS.EXE CATALOG.EXE AUDIO.CK4
# --- directories the box picked RIGHT: these must not change ---------------
mk RAPTOR   APOGEE.BAT RAP.EXE RAP-HELP.EXE DEALERS.EXE SWCBBS.EXE FILE0001.GLB
mk WACKY    APOGEE.BAT WW.EXE WW-HELP.EXE DEALERS.EXE SWCBBS.EXE WACKY.DAT
mk HEXEN    SERSETUP.EXE HEXEN.EXE DM.EXE IPXSETUP.EXE HEXEN.WAD
mk DOOM     DM.EXE DOOM.EXE DWANGO.EXE IPXSETUP.EXE DOOM1.WAD
mk EPICPIN  EP1.EXE PINBALL.EXE EPIC.DAT
mk DUKE     DN1.EXE DUKE.EXE DUKE.DN1
mk TYRIAN   TYRIAN.BAT TYRIAN2K.EXE TYRIAN.HDT
mk SW       SW.EXE COMMIT.EXE SWHELP.EXE SW.GRP
mk DUKE3D   COMMIT.EXE DN3DHELP.EXE DUKE3D.EXE DUKE3D.GRP
mk JAGGED2  DG.EXE MICPATCH.BAT JA2.DAT
# a lone SMALL exe with no data is a tiny complete game, not a download
mk TINY     TINY.EXE

# --- staged-library trees (GAMESYNC deploys these into C:\GAMES) -----------
# These are NOT shareware archives. They are the fleet's Windows-built staged
# trees, which carry a DOSBox of their own, several "Play <Game>.bat" wrappers
# and Win32 binaries beside the DOS ones. File lists copied from the real
# library, 2026-08-30.
mk QUAKE1   CWSDPMI.EXE GLQUAKE.EXE QLAUNCH.EXE QUAKE.EXE WINQUAKE.EXE Q95.BAT \
            PDIPX.COM PAK0.PAK
mk DESCENT1 DESCENT1.BAT DESCENT.BAT DESCENTR.EXE SETUP.EXE PCXVIEW.EXE \
            EREGCARD.EXE DESCENT.HOG
# the same two trees, with the declaration the library now ships
mk QUAKE1D  CWSDPMI.EXE GLQUAKE.EXE QLAUNCH.EXE QUAKE.EXE WINQUAKE.EXE Q95.BAT \
            PDIPX.COM PAK0.PAK
printf 'QUAKE.EXE\tQuake\n' > "$C/QUAKE1D/DOSGAME.TXT"
mk DESC1D   DESCENT1.BAT DESCENT.BAT DESCENTR.EXE SETUP.EXE PCXVIEW.EXE \
            EREGCARD.EXE DESCENT.HOG
printf '# comments and blanks are skipped\n\nDESCENTR.EXE\tDescent\n' \
    > "$C/DESC1D/DOSGAME.TXT"
# a declaration naming a file that is not there must NOT be honoured
mk BADDECL  DESCENTR.EXE DESCENT.HOG
printf 'NOTHERE.EXE\tGhost\n' > "$C/BADDECL/DOSGAME.TXT"

conf="$WORK/dosbox.conf"
{
    printf '[sdl]\noutput=surface\nautolock=false\n'
    printf '[dosbox]\nmachine=svga_s3\nmemsize=16\n'
    printf '[cpu]\ncore=auto\ncycles=max\n'
    printf '[mixer]\nnosound=true\n[sblaster]\nsbtype=none\n'
    printf '[speaker]\npcspeaker=false\ntandy=off\ndisney=false\n'
    printf '[autoexec]\nMOUNT C "%s"\nC:\ncd \\DOSGAME\n' "$C"
    printf 'DOSGAME.EXE /selftest\nexit\n'
} > "$conf"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy HOME="$WORK" \
    timeout -s KILL 180 "$DOSBOX" -conf "$conf" -userconf-skip >"$WORK/dosbox.log" 2>&1

SELF="$C/DOSGAME/DGSELF.TXT"
[ -f "$SELF" ] || { echo "  FAIL  no /selftest output"; exit 1; }

# expect <dir> <launcher> <why>
expect() {
    local got
    got=$(awk -F'|' -v d="C:\\\\$1" '$5==d {print $4}' "$SELF" | head -1)
    if [ "$got" = "$2" ]; then
        ok "$1 -> $2   ($3)"
    else
        bad "$1 -> ${got:-<missing>} , expected $2   ($3)"
    fi
}

echo "== launchers the box got wrong =="
expect KEENDRMS KEENDR.BAT  "was KEENDWEB.BAT, a web-download stub"
expect ROTT     ROTT.EXE    "was ROTTIPX.EXE, the IPX multiplayer launcher"
expect KEEN     KEEN4E.EXE  "was KEEN.EXE, an Apogee front-end shell"

echo "== launchers the box got right (must not regress) =="
expect RAPTOR   RAP.EXE     "catalogue names DEALERS.EXE/RAP-HELP.EXE, not this"
expect WACKY    WW.EXE      "ditto - the catalogue is not a clean oracle"
expect HEXEN    HEXEN.EXE   "DM.EXE is listed 7x but is the deathmatch launcher"
expect DOOM     DOOM.EXE    "named after its directory"
expect EPICPIN  EP1.EXE     "catalogue names both candidates, so first-found"
expect DUKE     DUKE.EXE    "DN1.EXE does not extend the directory name"
expect TYRIAN   TYRIAN.BAT  "a .BAT named for its directory is deliberate"
expect SW       SW.EXE      "dir-named and catalogued"
expect DUKE3D   DUKE3D.EXE  "dir-named"
expect JAGGED2  DG.EXE      "catalogue names it"
expect TINY     TINY.EXE    "small lone exe is a game, not a self-extractor"

echo "== DOSGAME.TXT: a staged tree declares its own DOS launcher =="
# WHY THIS PAIR. The first two assertions are the EVIDENCE for the feature, not
# a blessing of the guess: they record what the heuristic does with a staged
# Windows tree, which in both cases is something real DOS cannot start.
#   QUAKE1   -> GLQUAKE.EXE, a Win32 PE ("cannot be run in DOS mode")
#   DESCENT1 -> DESCENT1.BAT, a cmd.exe batch that opens with "cd /d"
# If either of these ever fails because pick_launcher got BETTER, that is good
# news - re-read this block rather than editing the expectation blind.
not_expect() {                      # not_expect <dir> <launcher> <why>
    local got
    got=$(awk -F'|' -v d="C:\\\\$1" '$5==d {print $4}' "$SELF" | head -1)
    if [ -n "$got" ] && [ "$got" != "$2" ]; then
        ok "$1 -> $got , NOT $2   ($3)"
    else
        bad "$1 -> ${got:-<missing>} , expected anything but $2   ($3)"
    fi
}
not_expect QUAKE1   QUAKE.EXE    "undeclared: the guess cannot find the DOS build"
not_expect DESCENT1 DESCENTR.EXE "undeclared: the guess cannot find the DOS build"
expect     QUAKE1D  QUAKE.EXE    "DOSGAME.TXT names the DOS build"
expect     DESC1D   DESCENTR.EXE "DOSGAME.TXT, past a comment and a blank line"
expect     BADDECL  DESCENTR.EXE "declaration names a missing file - guess instead"

# A declared TITLE reaches the menu, and is not overwritten by the catalogue's
# fuzzy name match (that is why the row leaves g->dir empty).
got=$(awk -F'|' -v d='C:\\DESC1D' '$5==d {print $3}' "$SELF" | head -1)
if [ "$got" = "Descent" ]; then
    ok "DESC1D titled \"Descent\" from DOSGAME.TXT field 2"
else
    bad "DESC1D titled \"${got:-<missing>}\", expected \"Descent\""
fi

echo "== known limitation: Jagged Alliance =="
# The real game is JA.EXE. NOTHING here can find it: no program is named after
# the directory (JAGGED~1), and the catalogue - which is the tie-breaker
# everywhere else - names DOXVIEW.EXE (2 rows) and EXECUTOR.EXE (5) but NOT
# JA.EXE, because gen_catalog.py picks "the shallowest non-installer exe" out
# of the archive and got a documentation viewer. So do not assert a launcher
# here; assert only what IS true - the obvious junk is excluded - and leave F2
# as the answer. Asserting DOXVIEW.EXE would enshrine a mis-pick as correct.
got=$(awk -F'|' -v d='C:\\JAGGED1' '$5==d {print $4}' "$SELF" | head -1)
case "$got" in
    CBYTES4.COM|GET.COM|"")
        bad "JAGGED1 -> ${got:-<missing>} , a tool rather than a program" ;;
    *)  ok "JAGGED1 -> $got   (not the junk; JA.EXE needs F2 - see comment)" ;;
esac

echo
echo "pick outcomes: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
