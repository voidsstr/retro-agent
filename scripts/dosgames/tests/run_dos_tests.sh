#!/bin/bash
# run_dos_tests.sh — DOS-side regression tests for DOSGAME.EXE, run headlessly
# in DOSBox on the dev host. No hardware, no keyboard, no network.
#
# Every test here pins a bug that was found on real hardware or measured in the
# emulator; see tests/README.md for the fix -> test table.
#
# Toolchain (override with env vars):
#   DOSGAME_WATCOM  Open Watcom v2 root (binl64/wcl)
#   DOSGAME_DOSBOX  a dosbox binary that runs with SDL_VIDEODRIVER=dummy
#
# Usage: bash tests/run_dos_tests.sh
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="$(dirname "$HERE")"
WORK="${DOSGAME_WORK:-/tmp/dosgame-tests-$$}"
WATCOM="${DOSGAME_WATCOM:-$HOME/development/toolchain-dos/watcom}"
DOSBOX="${DOSGAME_DOSBOX:-dosbox}"

pass=0; fail=0; skipped=0
ok()   { echo "  PASS  $*"; pass=$((pass+1)); }
bad()  { echo "  FAIL  $*"; fail=$((fail+1)); }
skip() { echo "  SKIP  $*"; skipped=$((skipped+1)); }

if [ ! -x "$WATCOM/binl64/wcl" ]; then
    echo "Open Watcom not found at $WATCOM (set DOSGAME_WATCOM) - skipping DOS tests."
    exit 0
fi
if ! command -v "$DOSBOX" >/dev/null 2>&1 && [ ! -x "$DOSBOX" ]; then
    echo "dosbox not found (set DOSGAME_DOSBOX) - skipping DOS tests."
    exit 0
fi

export WATCOM
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE="$WATCOM/h"

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------- build ----
echo "== build =="
BUILD_LOG="$WORK/build.log"
( cd "$SRCDIR" && wcl -bcl=dos -ml -os -q -wx -k8192 \
    -fe="$WORK/DOSGAME.EXE" dosgame.c ) > "$BUILD_LOG" 2>&1
if [ ! -f "$WORK/DOSGAME.EXE" ]; then
    echo "BUILD FAILED:"; cat "$BUILD_LOG"; exit 1
fi
if grep -q "Warning" "$BUILD_LOG"; then
    bad "build is warning-free"; cat "$BUILD_LOG"
else
    ok "build is warning-free"
fi
rm -f "$SRCDIR/dosgame.o"

# The 2K default stack was a real hazard for the scan call chain on hardware.
( cd "$SRCDIR" && wcl -bcl=dos -ml -os -q -k8192 \
    -fm="$WORK/dosgame.map" -fe="$WORK/map.exe" dosgame.c ) >/dev/null 2>&1
rm -f "$SRCDIR/dosgame.o"
if grep -qiE "Stack size: *2000 " "$WORK/dosgame.map"; then
    ok "linked stack is 8K (not Watcom's 2K default)"
else
    bad "linked stack is not 8K: $(grep -i 'stack size' "$WORK/dosgame.map")"
fi

# ------------------------------------------------------------ dosbox run ---
run_dos() {   # run_dos <croot> <cmd...>
    local croot="$1"; shift
    local conf="$WORK/dosbox.conf"
    {
        printf '[sdl]\noutput=surface\nautolock=false\n'
        printf '[dosbox]\nmachine=svga_s3\nmemsize=16\n'
        printf '[cpu]\ncore=auto\ncycles=max\n'
        printf '[mixer]\nnosound=true\n'
        printf '[sblaster]\nsbtype=none\n'
        printf '[speaker]\npcspeaker=false\ntandy=off\ndisney=false\n'
        printf '[autoexec]\n'
        printf 'MOUNT C "%s"\nC:\n' "$croot"
        # A .BAT must be CALLed. Chaining to one from [autoexec] abandons the
        # rest of it - the same gotcha the program itself works around - so the
        # trailing "exit" never ran and every fixture that used a T.BAT sat at
        # the DOS prompt until the timeout killed it. That is 90 s per test,
        # and it also meant SIGTERM (which DOSBox ignores here) decided when
        # the run ended rather than the test doing so.
        for c in "$@"; do
            case "$c" in
                *.BAT|*.bat) printf 'call %s\n' "$c" ;;
                *)           printf '%s\n' "$c" ;;
            esac
        done
        printf 'exit\n'
    } > "$conf"
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy HOME="$WORK" \
        timeout -s KILL 90 "$DOSBOX" -conf "$conf" -userconf-skip >"$WORK/dosbox.log" 2>&1
    return 0
}

# ============================================================ TEST 1 =======
# The bug the user hit: a kind 'I' game whose installer installs into its OWN
# directory must still end up PLAYABLE in the menu, and the leftover unpack
# directory must NOT be offered as "run setup" forever.
echo "== install -> play (installer installs elsewhere) =="
C="$WORK/e2e"
rm -rf "$C"; mkdir -p "$C/DOSGAME" "$C/GAMES" "$C/SHARE"
cp "$WORK/DOSGAME.EXE" "$C/DOSGAME/DOSGAME.EXE"
: > "$C/DOSGAME/QUIET.FLG"          # unattended: skip the "press a key" prompts
printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES;C:\\\ndrive=C:\\SHARE\n' \
    > "$C/DOSGAME/DOSGAME.CFG"
printf 'Wolfenstein 3D|WOLF3D.ZIP|I|WOLF3D.EXE|1234|WOLF.PRV\n' \
    > "$C/DOSGAME/GAMES.CAT"

cat > "$WORK/UNZIP.C" <<'EOF'
/* stub Info-ZIP: honours "-d <dir>" and drops an installer-style payload */
#include <stdio.h>
#include <string.h>
#include <direct.h>
int main(int argc,char**argv){
    int i; char *dir=0; char p[128]; FILE *f,*s; char buf[512]; size_t n;
    for(i=1;i<argc;i++) if(!strcmp(argv[i],"-d") && i+1<argc) dir=argv[i+1];
    if(!dir) return 1;
    mkdir(dir);
    sprintf(p,"%s\\INSTALL.EXE",dir);
    s=fopen("C:\\SHARE\\INSTALL.EXE","rb"); f=fopen(p,"wb");
    if(!s||!f) return 1;
    while((n=fread(buf,1,sizeof buf,s))>0) fwrite(buf,1,n,f);
    fclose(s); fclose(f);
    sprintf(p,"%s\\WOLF3D.1",dir);
    f=fopen(p,"wb"); if(f){fputs("PACKED",f);fclose(f);}
    return 0;
}
EOF
cat > "$WORK/INSTALL.C" <<'EOF'
/* stands in for a real Apogee/id INSTALL.EXE: installs to a directory of ITS
   own choosing, and leaves a decoy exe that sorts before the real one */
#include <stdio.h>
#include <direct.h>
int main(void){
    FILE*f; mkdir("C:\\WOLF3D");
    f=fopen("C:\\WOLF3D\\WOLF3D.EXE","wb"); if(f){fputs("MZ",f);fclose(f);}
    f=fopen("C:\\WOLF3D\\SETSOUND.EXE","wb"); if(f){fputs("MZ",f);fclose(f);}
    return 0;
}
EOF
( cd "$WORK" && wcl -bcl=dos -ml -os -q -fe="$C/DOSGAME/UNZIP.EXE" UNZIP.C \
  && wcl -bcl=dos -ml -os -q -fe="$C/SHARE/INSTALL.EXE" INSTALL.C ) >/dev/null 2>&1
printf 'PK' > "$C/SHARE/WOLF3D.ZIP"

cat > "$C/T.BAT" <<'EOF'
@echo off
C:\DOSGAME\DOSGAME.EXE /install:Wolfenstein
if errorlevel 42 call C:\DOSGAME\RUN.BAT
C:\DOSGAME\DOSGAME.EXE /selftest
C:\DOSGAME\DOSGAME.EXE /play:Wolfenstein
if errorlevel 42 echo PLAYABLE > C:\PLAYOK.TXT
EOF
run_dos "$C" "C:\\T.BAT"

REG="$C/DOSGAME/INSTALL.LST"
SELF="$C/DOSGAME/DGSELF.TXT"
if [ -f "$REG" ] && grep -qi '^G|Wolfenstein 3D|C:\\WOLF3D|WOLF3D.EXE' "$REG"; then
    ok "registry records the directory the INSTALLER created, with the right exe"
else
    bad "registry missing/incorrect: $(cat "$REG" 2>/dev/null)"
fi
if [ -f "$REG" ] && grep -qi '^X|' "$REG"; then
    ok "spent unpack directory is marked hidden"
else
    bad "spent unpack directory not marked hidden"
fi
if [ -f "$SELF" ] && grep -qi '^1|R|Wolfenstein 3D|WOLF3D.EXE|C:\\WOLF3D' "$SELF"; then
    ok "game is listed as installed + ready-to-run under its real title"
else
    bad "game not listed as playable: $(grep -i wolf "$SELF" 2>/dev/null)"
fi
# The regression itself: the unpack dir must not come back as a 'run setup' row.
if [ -f "$SELF" ] && grep -qiE '^1\|I\|WOLF' "$SELF"; then
    bad "leftover unpack dir is still offered as 'run setup' (the original bug)"
else
    ok "leftover unpack dir is no longer offered as 'run setup'"
fi
if [ -f "$C/PLAYOK.TXT" ]; then
    ok "/play writes a launch script for the installed game"
else
    bad "/play did not produce a launch script"
fi
if [ -f "$C/DOSGAME/RUN.BAT" ] && grep -qi 'cd \\WOLF3D' "$C/DOSGAME/RUN.BAT"; then
    ok "launch script cd's to the real game directory"
else
    bad "launch script does not cd to the game directory"
fi

# ============================================================ TEST 2 =======
# Non-flat archive: the game sits one directory below the unpack dir. The scan
# used to look only at depth 1, so those games were listed nowhere at all.
echo "== non-flat archive (game one level down) =="
C2="$WORK/deep"
rm -rf "$C2"; mkdir -p "$C2/DOSGAME" "$C2/GAMES/MYGAME/GAME"
cp "$WORK/DOSGAME.EXE" "$C2/DOSGAME/DOSGAME.EXE"
printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES\n' > "$C2/DOSGAME/DOSGAME.CFG"
printf 'MZ' > "$C2/GAMES/MYGAME/GAME/PLAY.EXE"
run_dos "$C2" "C:\\DOSGAME\\DOSGAME.EXE /selftest"
if grep -qi 'C:\\GAMES\\MYGAME\\GAME' "$C2/DOSGAME/DGSELF.TXT" 2>/dev/null; then
    ok "game one level below the unpack dir is found"
else
    bad "nested game not found: $(cat "$C2/DOSGAME/DGSELF.TXT" 2>/dev/null)"
fi

# ============================================================ TEST 3 =======
# Stem uniqueness. A plain 8-char truncation collapsed 1,268 of the 2,982
# catalogue rows onto shared install directories.
echo "== install-stem uniqueness =="
C3="$WORK/stem"
rm -rf "$C3"; mkdir -p "$C3/DOSGAME"
cp "$WORK/DOSGAME.EXE" "$C3/DOSGAME/DOSGAME.EXE"
printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES\n' > "$C3/DOSGAME/DOSGAME.CFG"
python3 - "$SRCDIR/data/GAMES.CAT" "$C3/DOSGAME/GAMES.CAT" <<'PY'
import sys
rows=[l for l in open(sys.argv[1],encoding='utf-8',errors='replace')
      if l.strip() and not l.startswith('#')]
duke=[l for l in rows if l.upper().startswith('DUKE') or 'DUKE NUKEM' in l.upper()]
out=(duke+rows)[:200]
open(sys.argv[2],'w',encoding='utf-8').writelines(out)
PY
run_dos "$C3" "C:\\DOSGAME\\DOSGAME.EXE /selftest"
# rows-loaded, stems-differing-from-the-server, install-dir-collisions
STEMOUT=$(python3 - "$C3/DOSGAME/DGSELF.TXT" "$SRCDIR/serve_dosgames.py" <<'PY'
import sys, importlib.util, collections
spec = importlib.util.spec_from_file_location("srv", sys.argv[2])
srv = importlib.util.module_from_spec(spec); spec.loader.exec_module(srv)
rows=[l.rstrip('\n').split('|') for l in open(sys.argv[1],encoding='cp437')]
cat=[f for f in rows if len(f)>=6 and f[0]=='0']
mismatch=sum(1 for f in cat if srv.zip_stem(f[4])!=f[5])
dupes=sum(1 for k,v in collections.Counter(f[5] for f in cat).items() if v>1)
print("%d %d %d" % (len(cat), mismatch, dupes))
PY
)
set -- $STEMOUT
if [ "${1:-0}" -gt 50 ]; then ok "catalog rows loaded for stem check ($1)"
else bad "too few catalog rows loaded ($STEMOUT)"; fi
if [ "${2:-1}" -eq 0 ]; then ok "DOS zip_stem() matches serve_dosgames.py exactly"
else bad "$2 stems differ between dosgame.c and serve_dosgames.py"; fi
if [ "${3:-1}" -eq 0 ]; then ok "no two catalog rows share an install directory"
else bad "$3 install-directory collisions"; fi

# ============================================================ TEST 4 =======
# A long scan= root used to overflow an 81-byte stack buffer in scan_game_dir
# via the unbounded path_join() - a hang at startup, before the UI appears.
# The program must survive and still produce its selftest dump.
echo "== overlong scan root (path_join bound) =="
C4="$WORK/longroot"
rm -rf "$C4"; mkdir -p "$C4/DOSGAME" "$C4/GAMES/OK"
cp "$WORK/DOSGAME.EXE" "$C4/DOSGAME/DOSGAME.EXE"
printf 'MZ' > "$C4/GAMES/OK/OK.EXE"
LONG='C:\THIS\IS\A\VERY\DEEPLY\NESTED\DOWNLOADS\DIRECTORY\FOR\OLD\DOS\GAMES\SHAREWARE\COLLECTION\APOGEE\NINETEEN\NINETYTHREE'
printf 'gamedir=C:\\GAMES\nscan=%s;C:\\GAMES\n' "$LONG" > "$C4/DOSGAME/DOSGAME.CFG"
run_dos "$C4" "C:\\DOSGAME\\DOSGAME.EXE /selftest"
if [ -f "$C4/DOSGAME/DGSELF.TXT" ]; then
    ok "survives an overlong scan= root and still completes the scan"
else
    bad "overlong scan= root killed the program (no DGSELF.TXT)"
fi
if grep -qi 'C:\\GAMES\\OK' "$C4/DOSGAME/DGSELF.TXT" 2>/dev/null; then
    ok "the good scan root is still scanned alongside the overlong one"
else
    bad "the good scan root was lost"
fi

# ========================================================= TEST 4b =========
# The diagnostic log. This is the only evidence anyone can collect after a
# failure in MS-DOS mode, so it has to actually contain the decisions -
# and the batch half of the story has to land in the same file.
echo "== diagnostic log =="
LOG="$C/DOSGAME/DOSGAME.LOG"
if [ -f "$LOG" ]; then
    ok "DOSGAME.LOG is written"
else
    bad "no DOSGAME.LOG produced"
fi
log_has() {   # log_has <what> <grep -E pattern>
    if grep -qiE "$2" "$LOG" 2>/dev/null; then ok "log records $1"
    else bad "log is missing $1"; fi
}
log_has "the config it loaded"            'config: (home|scan)='
log_has "the install decision + stem"     'install: stem='
log_has "which launcher the scan picked"  '^.*pick: '
log_has "the post-install reconciliation" 'post: +(OK|FAILED)'
log_has "what it wrote to the registry"   'registry: RECORD'
log_has "the batch steps (run: lines)"    '^run: '
# every helper pass must identify itself, or the narrative cannot be followed
for tag in menu snap post; do
    if grep -qE "^[0-9:]+ $tag " "$LOG" 2>/dev/null; then
        ok "log distinguishes the '$tag' pass"
    else
        bad "log has no '$tag' entries"
    fi
done

# ========================================================= TEST 4c =========
# The url= fetch path, using the real catalog. The generated command line must
# stay inside the DOS command tail, and the log must say how close it came -
# a silently truncated URL is what made 845 of 2,982 titles "fail to download".
echo "== url= install script (command-tail budget) =="
C5="$WORK/urlcfg"
rm -rf "$C5"; mkdir -p "$C5/DOSGAME" "$C5/GAMES"
cp "$WORK/DOSGAME.EXE" "$C5/DOSGAME/DOSGAME.EXE"
: > "$C5/DOSGAME/QUIET.FLG"
printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES\nurl=http://192.168.1.82:8181\n' \
    > "$C5/DOSGAME/DOSGAME.CFG"
head -200 "$SRCDIR/data/GAMES.CAT" > "$C5/DOSGAME/GAMES.CAT"
run_dos "$C5" 'C:\DOSGAME\DOSGAME.EXE /install:Air'
URLLOG="$C5/DOSGAME/DOSGAME.LOG"
if grep -qiE 'command tail is [0-9]+ bytes' "$URLLOG" 2>/dev/null; then
    ok "log records the fetch command-tail length"
else
    bad "log does not record the command-tail length"
fi
if [ -f "$C5/DOSGAME/RUN.BAT" ] && grep -qi 'HTGET .*/z/' "$C5/DOSGAME/RUN.BAT"; then
    ok "url= install fetches the short /z/<STEM> form"
else
    bad "url= install did not emit a /z/<STEM> fetch"
fi
# the emitted HTGET argument tail must fit; DOS truncates it silently at 126
TAIL=$(grep -i 'HTGET ' "$C5/DOSGAME/RUN.BAT" 2>/dev/null | head -1 | sed 's/^[^ ]* //' | tr -d '\r')
if [ -n "$TAIL" ] && [ "${#TAIL}" -le 126 ]; then
    ok "HTGET argument tail is ${#TAIL} bytes (DOS limit 126)"
else
    bad "HTGET argument tail is ${#TAIL:-0} bytes - over the DOS limit"
fi

# ========================================================= TEST 4d =========
# The Apogee multi-disk case. INSTALL.EXE plus <NAME>._1/._2 is the floppy-era
# shareware layout; when the installer is not answered it leaves those files
# behind. Blake Stone and Keen 4 failed this way on the Win98 box and the
# reconciliation blamed a "bad download", which sent the operator looking in
# entirely the wrong place. It must name what actually happened.
echo "== multi-disk installer diagnosis =="
C6="$WORK/diskset"
rm -rf "$C6"; mkdir -p "$C6/DOSGAME" "$C6/GAMES/BLAKE"
cp "$WORK/DOSGAME.EXE" "$C6/DOSGAME/DOSGAME.EXE"
printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES;C:\\\n' > "$C6/DOSGAME/DOSGAME.CFG"
: > "$C6/DOSGAME/QUIET.FLG"
printf 'MZ' > "$C6/GAMES/BLAKE/INSTALL.EXE"
head -c 2048 /dev/zero > "$C6/GAMES/BLAKE/BS_1BBS._1"
head -c 2048 /dev/zero > "$C6/GAMES/BLAKE/BS_1BBS._2"
printf 'BLAKE\r\nC:\\GAMES\\BLAKE\r\n\r\n\r\n' > "$C6/DOSGAME/PEND.SAV"
cat > "$C6/T.BAT" <<'EOF'
@echo off
C:\DOSGAME\DOSGAME.EXE /snapdirs
copy C:\DOSGAME\PEND.SAV C:\DOSGAME\PENDING.TXT > nul
C:\DOSGAME\DOSGAME.EXE /postinst
if errorlevel 1 echo FAILED > C:\RC.TXT
EOF
run_dos "$C6" 'C:\T.BAT'
DSLOG="$C6/DOSGAME/DOSGAME.LOG"
if [ -f "$C6/RC.TXT" ]; then
    ok "an unfinished multi-disk install is reported as a failure"
else
    bad "an unfinished multi-disk install was reported as success"
fi
if grep -qi "disk-set file(s) still present" "$DSLOG" 2>/dev/null; then
    ok "the leftover disk files are recognised"
else
    bad "leftover ._1/._2 disk files were not recognised"
fi
if grep -qi "multi-disk installer and it did not finish" "$DSLOG" 2>/dev/null; then
    ok "the message names the real cause, not a bad download"
else
    bad "the failure message still misdiagnoses this as a bad download"
fi
if grep -qiF 'answer C:\GAMES\BLAKE' "$DSLOG" 2>/dev/null; then
    ok "the message says what to answer the installer"
else
    bad "the message does not say what to answer"
fi
# and the directory listing must be in the log for remote diagnosis
if grep -qi "BS_1BBS._1" "$DSLOG" 2>/dev/null; then
    ok "the unpack directory contents are logged for diagnosis"
else
    bad "the unpack directory contents were not logged"
fi

# ============================================================ TEST 5 =======
# A stale RUN.BAT must never be re-run: DOSGAME.BAT deletes it each pass, so a
# menu that exits any other way cannot replay whatever the user last did.
echo "== series shell vs episode binary (KEEN.EXE / KEEN4E.EXE) =="
# C:\KEEN on the Win98 box held a 642K KEEN.EXE (an Apogee front-end) beside the
# 105K KEEN4E.EXE that is actually Commander Keen 4. "Named after its directory"
# picked the shell, so launching an installed game re-ran something
# installer-shaped instead of playing it.
grep -q 'is_util_suffix' "$SRCDIR/dosgame.c" \
  && ok "a util-looking suffix cannot outrank the game" \
  || bad "a util-looking suffix cannot outrank the game"
grep -q 'extends the directory name' "$SRCDIR/dosgame.c" \
  && ok "an exe extending the directory name beats the bare one" \
  || bad "an exe extending the directory name beats the bare one"
grep -q 'stricmp(bdot, ".BAT") != 0' "$SRCDIR/dosgame.c" \
  && ok "a .BAT named for its directory still wins (TYRIAN.BAT)" \
  || bad "a .BAT named for its directory still wins (TYRIAN.BAT)"
grep -q 'runnable programs' "$SRCDIR/dosgame.c" \
  && ok "every runnable candidate is logged, not just the winner" \
  || bad "every runnable candidate is logged, not just the winner"

# C:\HERETIC on the box held ONE 1.4MB HTIC_V10.EXE and no data at all - a
# download that was never extracted. It registered as ready-to-play, so Enter
# ran the installer instead of the game.
grep -q 'lone program, no data files' "$SRCDIR/dosgame.c" \
  && ok "a lone program with no data is treated as an unextracted download" \
  || bad "a lone program with no data is treated as an unextracted download"
grep -q 'nrun == 1 && ndata == 0' "$SRCDIR/dosgame.c" \
  && ok "the unextracted-download test needs both no data and one program" \
  || bad "the unextracted-download test needs both no data and one program"
# The size floor keeps a small lone exe (a tiny complete game, and the nested
# fixture below) from being misread as a download that needs extracting.
grep -q 'SELFEXTRACT_MIN_BYTES' "$SRCDIR/dosgame.c" \
  && ok "a lone exe must also be LARGE to count as a self-extractor" \
  || bad "a lone exe must also be LARGE to count as a self-extractor"

# The name-shape rule alone gets ROTT exactly BACKWARDS: it promoted
# ROTTIPX.EXE (the IPX multiplayer launcher) over ROTT.EXE (the game) on the
# real box. The catalogue lists ROTT.EXE and KEEN4E.EXE, and neither
# ROTTIPX.EXE nor KEEN.EXE, so it settles both cases correctly.
grep -q 'catalog_prefers' "$SRCDIR/dosgame.c" \
  && ok "the catalogue breaks the tie before the name-shape guess" \
  || bad "the catalogue breaks the tie before the name-shape guess"
grep -q 'cat_bits_build' "$SRCDIR/dosgame.c" \
  && ok "the catalogue is indexed ONCE, not re-read per directory" \
  || bad "the catalogue is indexed ONCE, not re-read per directory"
# A hash hit is probabilistic, so a false positive must only ever fall back to
# the old heuristic - never pick a launcher on its own.
grep -q 'return 0;                           /\* both or neither: caller decides \*/' "$SRCDIR/dosgame.c" \
  && ok "an ambiguous catalogue answer defers to the caller" \
  || bad "an ambiguous catalogue answer defers to the caller"

# "first non-tool .EXE" means "whatever order DOS returned", which is not a
# decision: it gave JAGGED~1 -> CBYTES4.COM and KEENDRMS -> KEENDWEB.BAT on the
# box, where the catalogue names DOXVIEW.EXE and KEENDR.BAT.
grep -q 'the only one of %d programs the catalogue' "$SRCDIR/dosgame.c" \
  && ok "an unambiguous catalogue name beats first-found" \
  || bad "an unambiguous catalogue name beats first-found"
grep -q 'hits > 1' "$SRCDIR/dosgame.c" \
  && ok "two catalogue matches stay a fallback, never a guess" \
  || bad "two catalogue matches stay a fallback, never a guess"
# The Apogee advertising bundle ships in every shareware directory and the
# catalogue itself names DEALERS.EXE/RAP-HELP.EXE/3DRCAT.EXE as launchers, so
# these have to be excluded by name and by shape.
for t in apogee.bat dealers.exe swcbbs.exe 3drcat.exe cbytes4.com sersetup.exe; do
  grep -q "\"$t\"" "$SRCDIR/dosgame.c" \
    && ok "$t is not treated as a game" \
    || bad "$t is not treated as a game"
done
grep -q 'help.exe' "$SRCDIR/dosgame.c" \
  && ok "per-game help viewers (RAP-HELP.EXE, DN3DHELP.EXE) are excluded by shape" \
  || bad "per-game help viewers are excluded by shape"

echo "== COMMAND.COM parses redirection inside rem, too =="
# The box grew 0-byte files named 43 in C:\, C:\KEEN, C:\DUKE and
# C:\GAMES\HERETIC. DOSGAME.BAT's own comment was the cause: COMMAND.COM saw
# the angle bracket, skipped the = (an argument delimiter), and created a file
# named 43 in whatever directory the menu loop was standing in - RUN.BAT cd's
# into the game's directory and a batch cd persists into its caller.
bad_rem=0
for f in "$SRCDIR"/*.BAT; do
  if grep -nE '^[[:space:]]*rem .*[<>]' "$f" >/dev/null 2>&1; then
    bad_rem=1
  fi
done
if [ "$bad_rem" = "0" ]; then
  ok "no shipped .BAT has an angle bracket in a rem comment"
else
  bad "a rem comment contains an angle bracket (COMMAND.COM will redirect)"
fi

echo "== a game installed under two scan roots is not silently deleted =="
# scan=C:\GAMES;C:\ made the de-dup drop C:\ROTT (playable) in favour of
# C:\GAMES\ROTT (an installer stub) - five games at once, with no log line.
grep -q 'same_path' "$SRCDIR/dosgame.c" \
  && ok "de-dup keys on the path, not the 8.3 directory name" \
  || bad "de-dup keys on the path, not the 8.3 directory name"
grep -q 'is playable - it replaces' "$SRCDIR/dosgame.c" \
  && ok "a playable install outranks a run-setup stub of the same name" \
  || bad "a playable install outranks a run-setup stub of the same name"
grep -q 'keeping ' "$SRCDIR/dosgame.c" \
  && ok "two equally playable copies are both kept, not guessed between" \
  || bad "two equally playable copies are both kept"
grep -q 'scan:   dropped' "$SRCDIR/dosgame.c" \
  && ok "every dropped entry is logged" \
  || bad "every dropped entry is logged"

echo "== generated RUN.BAT lines fit COMMAND.COM's line buffer =="
grep -q 'BAT_LINE_MAX' "$SRCDIR/dosgame.c" \
  && ok "the batch line limit is a named constant" \
  || bad "the batch line limit is a named constant"
grep -q 'emit_log_p' "$SRCDIR/dosgame.c" \
  && ok "a line's existing prefix counts against the limit" \
  || bad "a line's existing prefix counts against the limit"

echo "== install directory join is bounded =="
# cfg_gamedir is 79 operator-supplied chars and dir[] is 81 bytes; the old
# sprintf smashed write_install's own frame, and real mode has no guard rail.
grep -q 'path_join(dir, cfg_gamedir, stem)' "$SRCDIR/dosgame.c" \
  && ok "write_install joins through the bounded helper" \
  || bad "write_install joins through the bounded helper"
if grep -q 'sprintf(dir, "%s' "$SRCDIR/dosgame.c"; then
  bad "the unbounded sprintf into dir[] is still there"
else
  ok "the unbounded sprintf into dir[] is gone"
fi

echo "== a maximum-length PENDING.TXT field does not break reconciliation =="
# fgets(buf, n) stores n-1 chars and stops WITHOUT consuming the newline, so a
# title of exactly MAX_TITLE (40) left the '\n' in the stream: the next read
# returned just that newline, unpack came out empty, and post_install reported
# "PENDING.TXT incomplete" for an install that had succeeded. 123 of the
# shipped catalogue's 2,982 titles are exactly 40 characters. The same
# off-by-one on want[13] blanked the tile for the 459 rows whose launcher is a
# full 12-character 8.3 name.
C6="$WORK/pending"
rm -rf "$C6"; mkdir -p "$C6/DOSGAME" "$C6/GAMES/BATTLE01"
cp "$WORK/DOSGAME.EXE" "$C6/DOSGAME/DOSGAME.EXE"
printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES;C:\\\n' > "$C6/DOSGAME/DOSGAME.CFG"
printf 'MZ' > "$C6/GAMES/BATTLE01/BOB.EXE"
printf 'x'  > "$C6/GAMES/BATTLE01/BOB.DAT"
T40="Battle Of Britain 1940 Their Finest Hour"
[ ${#T40} -eq 40 ] || bad "the fixture title is not 40 characters (${#T40})"
printf '%s\r\nC:\\GAMES\\BATTLE01\r\nHTIC_V10.EXE\r\nBATTLE01.PRV\r\n' "$T40" \
    > "$C6/DOSGAME/PENDING.TXT"
run_dos "$C6" "C:\\DOSGAME\\DOSGAME.EXE /postinst"

if grep -q "incomplete" "$C6/DOSGAME/DOSGAME.LOG" 2>/dev/null; then
    bad "a 40-char title still reports PENDING.TXT incomplete"
else
    ok "a maximum-length title is read without losing the next field"
fi
if grep -qi '^G|.*|C:\\GAMES\\BATTLE01|BOB.EXE' "$C6/DOSGAME/INSTALL.LST" 2>/dev/null; then
    ok "the install is recorded as playable, not as a failure"
else
    bad "no G row: $(cat "$C6/DOSGAME/INSTALL.LST" 2>/dev/null)"
fi
if grep -qi 'BATTLE01.PRV' "$C6/DOSGAME/INSTALL.LST" 2>/dev/null; then
    ok "the tile survives a 12-character launcher field"
else
    bad "the tile was lost after a 12-character launcher field"
fi

echo "== an installer is never recorded as the game =="
# gen_catalog emitted `exe or "INSTALL.EXE"`, and a 'G' row hides its directory
# from every later scan - so recording the installer meant Enter re-ran it
# forever, with no snapshot and no reconciliation.
C7="$WORK/instexe"
rm -rf "$C7"; mkdir -p "$C7/DOSGAME" "$C7/GAMES/WOLF01"
cp "$WORK/DOSGAME.EXE" "$C7/DOSGAME/DOSGAME.EXE"
printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES;C:\\\n' > "$C7/DOSGAME/DOSGAME.CFG"
printf 'MZ' > "$C7/GAMES/WOLF01/INSTALL.EXE"
printf 'MZ' > "$C7/GAMES/WOLF01/WOLF3D.EXE"
printf 'x'  > "$C7/GAMES/WOLF01/WOLF3D.WL1"
printf 'Wolfenstein 3D\r\nC:\\GAMES\\WOLF01\r\nINSTALL.EXE\r\nWOLF01.PRV\r\n' \
    > "$C7/DOSGAME/PENDING.TXT"
run_dos "$C7" "C:\\DOSGAME\\DOSGAME.EXE /postinst"
if grep -qi '^G|Wolfenstein 3D|C:\\GAMES\\WOLF01|INSTALL.EXE' "$C7/DOSGAME/INSTALL.LST" 2>/dev/null; then
    bad "the installer was recorded as the game's launcher"
else
    ok "the catalogue's INSTALL.EXE is rejected as a launcher"
fi
if grep -qi '^G|Wolfenstein 3D|C:\\GAMES\\WOLF01|WOLF3D.EXE' "$C7/DOSGAME/INSTALL.LST" 2>/dev/null; then
    ok "the real game beside it is recorded instead"
else
    bad "the real game was not found: $(cat "$C7/DOSGAME/INSTALL.LST" 2>/dev/null)"
fi

echo "== F2 cannot lock an installer in as the game =="
grep -q 'is_skip_exe(ft.name) || is_setup_exe(ft.name)) continue' "$SRCDIR/dosgame.c" \
  && ok "F2's cycle skips installers and support tools" \
  || bad "F2's cycle skips installers and support tools"
grep -q "reg_append(g->kind == 'R' ? 'G' : 'S'" "$SRCDIR/dosgame.c" \
  && ok "an F2 choice on a needs-setup row keeps its class ('S')" \
  || bad "an F2 choice on a needs-setup row keeps its class"
grep -q "flag == 'S') ? 'I' : 'R'" "$SRCDIR/dosgame.c" \
  && ok "an 'S' row reloads as needs-setup, not ready-to-run" \
  || bad "an 'S' row reloads as needs-setup"

echo "== a game that merely RAN is not mistaken for where an install went =="
# Playing Duke Nukem 3D leaves DUKE3D.CFG and DD.CFG behind. Step 2b took the
# first directory touched since the snapshot, so C:\GAMES\DUKE3D answered yes to
# every later install: Blake Stone AND Shadow Warrior were both recorded as
# C:\GAMES\DUKE3D\DUKE3D.EXE and every game in the menu launched Duke Nukem.
mk_d3() {   # mk_d3 <croot> <new files...>
    local c="$1"; shift
    rm -rf "$c"; mkdir -p "$c/DOSGAME" "$c/GAMES/BLAKE" "$c/GAMES/DUKE3D"
    cp "$WORK/DOSGAME.EXE" "$c/DOSGAME/DOSGAME.EXE"
    printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES;C:\\\n' > "$c/DOSGAME/DOSGAME.CFG"
    printf 'MZ' > "$c/GAMES/BLAKE/INSTALL.EXE"
    printf 'x'  > "$c/GAMES/BLAKE/BS_1BBS._1"
    printf 'x'  > "$c/GAMES/BLAKE/BS_1BBS._2"
    for f in DUKE3D.EXE COMMIT.EXE SETUP.EXE DUKE3D.GRP GAME.CON USER.CON; do
        printf 'MZ' > "$c/GAMES/DUKE3D/$f"
    done
    printf 'Blake Stone\r\nC:\\GAMES\\BLAKE\r\n\r\n\r\n' > "$c/DOSGAME/PENDING.TXT"
    # DOS stamps have 2-second granularity: leave a gap the snapshot lands in.
    sleep 3
    { echo "@echo off"
      echo "C:\\DOSGAME\\DOSGAME.EXE /snapdirs"
      for f in "$@"; do echo "echo x > C:\\GAMES\\DUKE3D\\$f"; done
      echo "C:\\DOSGAME\\DOSGAME.EXE /postinst"
    } > "$c/T.BAT"
}

C8="$WORK/played"
mk_d3 "$C8" DUKE3D.CFG DD.CFG
run_dos "$C8" "C:\\T.BAT"
if grep -q "too few to be an install" "$C8/DOSGAME/DOSGAME.LOG" 2>/dev/null; then
    ok "two config files are not enough to claim a directory"
else
    bad "a played game's config files were taken as an install"
fi
if grep -qi 'DUKE3D' "$C8/DOSGAME/INSTALL.LST" 2>/dev/null; then
    bad "the played game was recorded as this game's launcher"
else
    ok "nothing was recorded, so the menu cannot launch the wrong game"
fi

C9="$WORK/installed"
mk_d3 "$C9" BSTONE.EXE AUDIOHED.BS1 MAPHEAD.BS1 VGAGRAPH.BS1 VSWAP.BS1
run_dos "$C9" "C:\\T.BAT"
if grep -q "is where the installer wrote" "$C9/DOSGAME/DOSGAME.LOG" 2>/dev/null; then
    ok "a real install into an existing directory is still found"
else
    bad "a real install into an existing directory was missed"
fi

echo "== DOS timestamps pack into 32 bits without wrapping =="
# The old packing shifted the FULL year left by 26; 1980 << 26 needs 37 bits,
# so on a 16-bit compiler the year wrapped modulo 64 and a 1980 file compared
# as NEWER than a 2026 one. It never bit on the fleet box only because its CMOS
# battery is dead and every stamp there is 1980.
grep -q 'ft->wr_date << 16' "$SRCDIR/dosgame.c" \
  && ok "file stamps use DOS's own packed date/time words" \
  || bad "file stamps use DOS's own packed date/time words"
grep -q 'd.year - 1980' "$SRCDIR/dosgame.c" \
  && ok "the clock stamp stores years since 1980, so it cannot overflow" \
  || bad "the clock stamp stores years since 1980"
if grep -qE 'year << 26|d\.year << 26' "$SRCDIR/dosgame.c"; then
    bad "the overflowing year shift is still there"
else
    ok "the overflowing year shift is gone"
fi

echo "== an orphaned X row stops hiding its directory =="
# An X row means "this unpack dir is spent, the game it produced is recorded
# elsewhere". When that game row goes, the X row is left hiding a directory
# whose install never finished - the user cannot even see it to retry. That is
# exactly the state .243 was left in after two games were recorded against the
# wrong directory and those rows were removed.
CA="$WORK/orphan"
rm -rf "$CA"; mkdir -p "$CA/DOSGAME" "$CA/GAMES/BLAKE" "$CA/GAMES/KEEN1" "$CA/KEEN"
cp "$WORK/DOSGAME.EXE" "$CA/DOSGAME/DOSGAME.EXE"
printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES;C:\\\n' > "$CA/DOSGAME/DOSGAME.CFG"
printf 'MZ' > "$CA/GAMES/BLAKE/INSTALL.EXE"
printf 'x'  > "$CA/GAMES/BLAKE/BS_1BBS._1"
printf 'MZ' > "$CA/GAMES/KEEN1/DEICE.EXE"
printf 'MZ' > "$CA/KEEN/KEEN4E.EXE"
printf 'x'  > "$CA/KEEN/AUDIO.CK4"
printf 'X|BLAKE|C:\\GAMES\\BLAKE||\r\nG|KEEN1|C:\\KEEN|KEEN4E.EXE|\r\nX|KEEN1|C:\\GAMES\\KEEN1||\r\n' \
    > "$CA/DOSGAME/INSTALL.LST"
run_dos "$CA" "C:\\DOSGAME\\DOSGAME.EXE /selftest"

if grep -q 'DROP X C:\\GAMES\\BLAKE' "$CA/DOSGAME/DOSGAME.LOG" 2>/dev/null; then
    ok "an X row with no game row is dropped"
else
    bad "an orphaned X row still hides its directory"
fi
if grep -qi '|BLAKE|' "$CA/DOSGAME/DGSELF.TXT" 2>/dev/null; then
    ok "the unfinished install is visible again, so it can be retried"
else
    bad "the unfinished install is still hidden: $(cat "$CA/DOSGAME/DGSELF.TXT" 2>/dev/null)"
fi
if grep -q 'X "KEEN1"' "$CA/DOSGAME/DOSGAME.LOG" 2>/dev/null; then
    ok "an X row that IS paired with a game row is kept"
else
    bad "a legitimate X row was dropped too"
fi

echo "== a rewound clock does not hand the install to an old game =="
# .243's CMOS battery is dead: its clock reads 1980 while the game files
# restored from the archives keep their original dates (C:\DUKE is 66 files
# stamped 11-01-91, C:\KEEN's are 02-01-92). Against a 1980 "since" every one
# of those vintage files counts as just-written, so C:\DUKE scored 66 and was
# handed EVERY install - the same "it launches the wrong game" symptom, one
# directory over. Neither the 3-file floor nor busiest-wins helps: the noise is
# in the tens. Only an upper bound does.
#
# DOSBox has no -date option, but future host mtimes reproduce the SAME
# relationship: existing files stamped later than the clock the box is running.
CB="$WORK/rtc"
rm -rf "$CB"; mkdir -p "$CB/DOSGAME" "$CB/GAMES/BLAKE" "$CB/DUKE" "$CB/BSTONE"
cp "$WORK/DOSGAME.EXE" "$CB/DOSGAME/DOSGAME.EXE"
printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES;C:\\\n' > "$CB/DOSGAME/DOSGAME.CFG"
printf 'MZ' > "$CB/GAMES/BLAKE/INSTALL.EXE"
printf 'x'  > "$CB/GAMES/BLAKE/BS_1BBS._1"
i=1
while [ $i -le 66 ]; do printf 'x' > "$CB/DUKE/DUKE$i.DAT"; i=$((i+1)); done
printf 'MZ' > "$CB/DUKE/DUKE.EXE"
touch -d "2030-01-01 12:00" "$CB/DUKE"/*
printf 'Blake Stone\r\nC:\\GAMES\\BLAKE\r\n\r\n\r\n' > "$CB/DOSGAME/PENDING.TXT"
{ echo "@echo off"
  echo "C:\\DOSGAME\\DOSGAME.EXE /snapdirs"
  for f in BSTONE.EXE AUDIOHED.BS1 MAPHEAD.BS1 VSWAP.BS1; do
      echo "echo x > C:\\BSTONE\\$f"
  done
  echo "C:\\DOSGAME\\DOSGAME.EXE /postinst"
} > "$CB/T.BAT"
run_dos "$CB" "C:\\T.BAT"

if grep -qi '^G|Blake Stone|C:\\BSTONE|' "$CB/DOSGAME/INSTALL.LST" 2>/dev/null; then
    ok "the directory the installer actually wrote to is chosen"
else
    bad "wrong directory chosen: $(cat "$CB/DOSGAME/INSTALL.LST" 2>/dev/null)"
fi
if grep -qi 'C:\\DUKE is where' "$CB/DOSGAME/DOSGAME.LOG" 2>/dev/null; then
    bad "66 untouched vintage files were counted as this install"
else
    ok "files stamped after the install cannot count as part of it"
fi
grep -q 'unsigned long until' "$SRCDIR/dosgame.c" \
  && ok "the install window is bounded at both ends" \
  || bad "the install window is bounded at both ends"

echo "== the multi-disk diagnosis reaches the SCREEN, not just the log =="
# logf() only writes DOSGAME.LOG. All the operator saw was RUN.BAT's generic
# "nothing runnable was found", or worse "the download may be damaged" - which
# sends them hunting a bad download when every disk is already on disk.
grep -q 'printf("\\n  This game came as a multi-disk set' "$SRCDIR/dosgame.c" \
  && ok "the multi-disk explanation is printed to stdout" \
  || bad "the multi-disk explanation is only logged"

echo "== DHCP is not aborted by a stray keystroke =="
# mTCP's DHCP takes ANY buffered key as its advertised "[ESC] to abort"; the
# box logged "attempt 1: Aborting" instantly, which reads like a dead NIC.
grep -q '/kflush' "$SRCDIR/dosgame.c" \
  && ok "DOSGAME.EXE has a headless /kflush mode" \
  || bad "DOSGAME.EXE has a headless /kflush mode"
grep -q 'mode_kflush' "$SRCDIR/dosgame.c" \
  && ok "/kflush runs before any video setup" \
  || bad "/kflush runs before any video setup"
[ "$(grep -c "DOSGAME.EXE /kflush" "$SRCDIR/NETUP.BAT")" = "2" ] \
  && ok "both DHCP call sites drain the keyboard first" \
  || bad "both DHCP call sites drain the keyboard first"
for n in $(grep -n 'DHCP.EXE >>'  "$SRCDIR/NETUP.BAT" | cut -d: -f1); do
  sed -n "$((n-1))p"  "$SRCDIR/NETUP.BAT" | grep -q '/kflush' \
    || bad "the drain is on the line immediately before DHCP (line $n)"
done
ok "the drain is on the line immediately before DHCP"

echo "== shipped DOSGAME.BAT hygiene =="
if grep -qi 'del C:\\DOSGAME\\RUN.BAT' "$SRCDIR/DOSGAME.BAT"; then
    ok "DOSGAME.BAT clears a stale RUN.BAT before each menu pass"
else
    bad "DOSGAME.BAT does not clear a stale RUN.BAT"
fi

# ======================================================= TEST 12 ===========
# Commander Keen 1 "would not install" on .243 (2026-08-25).
#
# keen1_shareware.zip is the Apogee BBS layout - DEICE.EXE, KEEN.1, KEEN.DAT
# and INSTALL.BAT - and the scan kept whichever installer-shaped file DOS
# happened to return FIRST, which was DEICE.EXE. DEICE on its own only rebuilds
# the packed KEEN.EXE self-extractor; the vendor's INSTALL.BAT is what then
# RUNS it. Enter therefore produced one file, /postinst called that "too few to
# be an install", and the game was never playable.
echo "== a DEICE set is entered through INSTALL.BAT, not DEICE.EXE =="
CD1="$WORK/deice"
rm -rf "$CD1"; mkdir -p "$CD1/DOSGAME" "$CD1/GAMES/KEEN1"
cp "$WORK/DOSGAME.EXE" "$CD1/DOSGAME/DOSGAME.EXE"
: > "$CD1/DOSGAME/QUIET.FLG"
printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES\n' > "$CD1/DOSGAME/DOSGAME.CFG"
printf 'MZ' > "$CD1/GAMES/KEEN1/DEICE.EXE"
printf '@echo off\r\nDEICE\r\n' > "$CD1/GAMES/KEEN1/INSTALL.BAT"
# a COMPLETE set: the parts add up to the SIZE the .DAT declares
head -c 4096 /dev/zero > "$CD1/GAMES/KEEN1/KEEN.1"
printf 'PATH=\\KEEN\r\nSIZE=4096\r\nEXPSIZE=350000\r\n' > "$CD1/GAMES/KEEN1/KEEN.DAT"
run_dos "$CD1" 'C:\DOSGAME\DOSGAME.EXE /selftest' 'C:\DOSGAME\DOSGAME.EXE /play:KEEN1'
if grep -qi '^1|I|KEEN1|INSTALL.BAT|' "$CD1/DOSGAME/DGSELF.TXT" 2>/dev/null; then
    ok "INSTALL.BAT is chosen over DEICE.EXE as the entry point"
else
    bad "the DEICE set still enters through: $(cat "$CD1/DOSGAME/DGSELF.TXT" 2>/dev/null)"
fi
if grep -qi '^call INSTALL.BAT' "$CD1/DOSGAME/RUN.BAT" 2>/dev/null; then
    ok "the generated script CALLs the vendor's INSTALL.BAT"
else
    bad "RUN.BAT does not call INSTALL.BAT: $(cat "$CD1/DOSGAME/RUN.BAT" 2>/dev/null)"
fi
grep -q 'static int setup_rank' "$SRCDIR/dosgame.c" \
  && ok "installer candidates are RANKED, not taken in directory order" \
  || bad "installer candidates are ranked, not taken in directory order"

# ======================================================= TEST 13 ===========
# "another game asked for disk 2 and would not install" - heretic_shareware1.zip
# on the share is 1.4 MB against the 2,863,638-byte set its own .DAT declares.
# It is disk 1 of two and disk 2 is not in the archive at all, so its installer
# stops at a floppy prompt no answer can satisfy. Refuse before starting it.
echo "== an incomplete multi-disk download is refused up front =="
CD2="$WORK/shortset"
rm -rf "$CD2"; mkdir -p "$CD2/DOSGAME" "$CD2/GAMES/HERETIC"
cp "$WORK/DOSGAME.EXE" "$CD2/DOSGAME/DOSGAME.EXE"
: > "$CD2/DOSGAME/QUIET.FLG"
printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES\n' > "$CD2/DOSGAME/DOSGAME.CFG"
printf 'MZ' > "$CD2/GAMES/HERETIC/DEICE.EXE"
printf '@echo off\r\nDEICE.EXE\r\n' > "$CD2/GAMES/HERETIC/INSTALL.BAT"
head -c 8192 /dev/zero > "$CD2/GAMES/HERETIC/HTIC_V10.1"
printf 'PATH=\\HERETIC\r\nSIZE=2863638\r\nEXPSIZE=6090000\r\n' \
    > "$CD2/GAMES/HERETIC/HTIC_V10.DAT"
# The branch must be a GOTO, not "if errorlevel 42 echo x > file": the shell
# sets up the redirection BEFORE evaluating the IF, so the file is created
# (0 bytes) even when the condition is false - the same parse-order trap that
# scattered stray "43" files over the box from a rem comment.
cat > "$CD2/T.BAT" <<'EOF'
@echo off
C:\DOSGAME\DOSGAME.EXE /play:HERETIC
if errorlevel 42 goto launched
goto done
:launched
echo LAUNCHED > C:\LAUNCH.TXT
:done
EOF
run_dos "$CD2" 'C:\T.BAT'
if [ -s "$CD2/LAUNCH.TXT" ]; then
    bad "an installer that can never finish was started anyway"
else
    ok "an installer whose disks are not all there is not started"
fi
if grep -qi 'launch: REFUSED' "$CD2/DOSGAME/DOSGAME.LOG" 2>/dev/null; then
    ok "the log says it refused, and why"
else
    bad "the refusal is not in the log: $(cat "$CD2/DOSGAME/DOSGAME.LOG" 2>/dev/null | tail -3)"
fi
# and a COMPLETE set must still be launchable - the check must not fire on
# every DEICE game, only on a genuinely short one.
if grep -qi 'launch: REFUSED' "$CD1/DOSGAME/DOSGAME.LOG" 2>/dev/null; then
    bad "a complete disk set was refused too"
else
    ok "a complete disk set is still launched"
fi

# ======================================================= TEST 14 ===========
# The disk-set counter only recognised "._<digit>". Every id/Apogee set on the
# share numbers its parts as the bare extension - KEEN.1, HTIC_V10.1 - so it
# counted ZERO disks and reported a stalled install as "the installer wrote
# nothing at all (cancelled, or the download is bad)".
echo "== NAME.1 is a disk-set part, not just NAME._1 =="
CD3="$WORK/dotone"
rm -rf "$CD3"; mkdir -p "$CD3/DOSGAME" "$CD3/GAMES/KEEN1"
cp "$WORK/DOSGAME.EXE" "$CD3/DOSGAME/DOSGAME.EXE"
printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES;C:\\\n' > "$CD3/DOSGAME/DOSGAME.CFG"
printf 'MZ' > "$CD3/GAMES/KEEN1/DEICE.EXE"
head -c 4096 /dev/zero > "$CD3/GAMES/KEEN1/KEEN.1"
printf 'KEEN1\r\nC:\\GAMES\\KEEN1\r\n\r\n\r\n' > "$CD3/DOSGAME/PENDING.TXT"
run_dos "$CD3" 'C:\DOSGAME\DOSGAME.EXE /snapdirs' 'C:\DOSGAME\DOSGAME.EXE /postinst'
if grep -qi 'disk-set file(s) still present' "$CD3/DOSGAME/DOSGAME.LOG" 2>/dev/null; then
    ok "a KEEN.1 style part counts as a disk-set file"
else
    bad "KEEN.1 was not recognised as a disk-set part"
fi
if grep -qi 'the download is bad' "$CD3/DOSGAME/DOSGAME.LOG" 2>/dev/null; then
    bad "a stalled multi-disk install is still blamed on a bad download"
else
    ok "a stalled multi-disk install is no longer blamed on a bad download"
fi

# ======================================================= TEST 15 ===========
# The network install "did not work correctly": on .243 a LAN install that
# succeeded end to end still logged "HTGET failed - is ... serving?" and
# "UNZIP failed - corrupt zip, disk full, or no DPMI" immediately above
# "install finished OK". ERRORLEVEL is not reliable in a COMMAND.COM chain
# (DOSGAME itself exits 42 to hand over to RUN.BAT); test the artifact.
echo "== install script judges the artifact, not ERRORLEVEL =="
RB="$C5/DOSGAME/RUN.BAT"
if grep -qiE '^if not exist .*\.ZIP echo run: +HTGET failed' "$RB" 2>/dev/null; then
    ok "the HTGET verdict is 'is the zip there?'"
else
    bad "the HTGET verdict is not an artifact test: $(grep -i htget "$RB" 2>/dev/null | tr -d '\r')"
fi
if grep -qiE '^if errorlevel 1 echo run:' "$RB" 2>/dev/null; then
    bad "a tool verdict is still taken from ERRORLEVEL"
else
    ok "no tool verdict is taken from ERRORLEVEL"
fi
# /postinst's exit code IS a real errorlevel and must still be branched on.
if grep -qi 'if errorlevel 1 goto nogame' "$RB" 2>/dev/null; then
    ok "/postinst's own exit code is still the install's verdict"
else
    bad "/postinst's verdict branch was lost"
fi

# ======================================================= TEST 16 ===========
# "game names should be the full name of the game in the list": a game found by
# the disk scan was listed by its DIRECTORY (KEEN1, STARCR~1, JAGGED~1) while
# the Available tab beside it listed the same game as "keen1 shareware".
echo "== an installed game is named after the catalogue =="
CD4="$WORK/titles"
rm -rf "$CD4"; mkdir -p "$CD4/DOSGAME" "$CD4/GAMES/KEEN1" "$CD4/GAMES/HEXEN"
cp "$WORK/DOSGAME.EXE" "$CD4/DOSGAME/DOSGAME.EXE"
printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES\n' > "$CD4/DOSGAME/DOSGAME.CFG"
printf 'MZ' > "$CD4/GAMES/KEEN1/DEICE.EXE"
printf '@echo off\r\n' > "$CD4/GAMES/KEEN1/INSTALL.BAT"
printf 'x' > "$CD4/GAMES/KEEN1/KEEN.1"
printf 'MZ' > "$CD4/GAMES/HEXEN/HEXEN.EXE"
printf 'x'  > "$CD4/GAMES/HEXEN/HEXEN.WAD"
{ printf 'keen1 shareware|keen1_shareware.zip|I|DEICE.EXE|209754|KEEN1_SH.PRV\n'
  printf 'Hexen Beyond Heretic|Hexen Beyond Heretic (1995).zip|I|D202.EXE|997769|HEXEN_BE.PRV\n'
  printf 'Hexen Deathkings Of The Dark Citadel|Hexen DK (1996).zip|I|D202.EXE|997769|HEXEN_DK.PRV\n'
} > "$CD4/DOSGAME/GAMES.CAT"
run_dos "$CD4" 'C:\DOSGAME\DOSGAME.EXE /selftest'
if grep -qi '^1|I|keen1 shareware|' "$CD4/DOSGAME/DGSELF.TXT" 2>/dev/null; then
    ok "the folder KEEN1 is listed under the catalogue's full title"
else
    bad "KEEN1 kept its folder name: $(cat "$CD4/DOSGAME/DGSELF.TXT" 2>/dev/null)"
fi
# Two Hexen rows could both be C:\HEXEN. A confidently wrong name is worse than
# a dull correct one, so a tie must keep the folder name.
if grep -qi '^1|R|HEXEN|' "$CD4/DOSGAME/DGSELF.TXT" 2>/dev/null; then
    ok "an ambiguous match keeps the folder name instead of guessing"
else
    bad "an ambiguous match was resolved anyway: $(grep -i hexen "$CD4/DOSGAME/DGSELF.TXT" 2>/dev/null)"
fi
if grep -qi 'title:  KEEN1 ->' "$CD4/DOSGAME/DOSGAME.LOG" 2>/dev/null; then
    ok "the log records what each folder was renamed to"
else
    bad "renames are not in the log"
fi

# ====================================================== TEST 16b ===========
# The catalogue title must follow its own row when scan_local SHUFFLES games[].
#
# Shipped in the previous fix and broken on the first real box. The folder name
# was kept in an array indexed alongside games[], but scan_local both
# OVERWRITES rows (games[j] = games[i], when a playable copy replaces a
# run-setup stub of the same 8.3 name) and MEMMOVEs the tail down when it drops
# a duplicate. On .243 - five such replacements under scan=C:\GAMES;C:\ - every
# key ended up against the wrong row and NOT ONE game got its title. On this
# fixture the same misalignment does something worse than nothing: it titled
# C:\STARCR~1 "Doom".
echo "== a catalogue title follows its own row when the scan shuffles rows =="
CD5="$WORK/shuffle"
rm -rf "$CD5"
mkdir -p "$CD5/DOSGAME" "$CD5/GAMES/ROTT" "$CD5/ROTT" \
         "$CD5/GAMES/DOOM" "$CD5/DOOM" "$CD5/GAMES/KEEN1" "$CD5/ZZUNIQ"
cp "$WORK/DOSGAME.EXE" "$CD5/DOSGAME/DOSGAME.EXE"
cp "$SRCDIR/data/GAMES.CAT" "$CD5/DOSGAME/GAMES.CAT"
printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES;C:\\\n' > "$CD5/DOSGAME/DOSGAME.CFG"
# C:\GAMES\<name> is the unpacked disk set, C:\<name> is what its installer
# produced - the shape that makes scan_local replace a row in place.
printf 'MZ' > "$CD5/GAMES/ROTT/INSTALL.EXE";  printf 'x' > "$CD5/GAMES/ROTT/ROTT.1"
printf 'MZ' > "$CD5/ROTT/ROTT.EXE";           printf 'x' > "$CD5/ROTT/ROTT.WAD"
printf 'MZ' > "$CD5/GAMES/DOOM/INSTALL.EXE";  printf 'x' > "$CD5/GAMES/DOOM/DOOM.1"
printf 'MZ' > "$CD5/DOOM/DOOM.EXE";           printf 'x' > "$CD5/DOOM/DOOM1.WAD"
printf 'MZ' > "$CD5/GAMES/KEEN1/DEICE.EXE"
printf '@echo off\r\n' > "$CD5/GAMES/KEEN1/INSTALL.BAT"
printf 'x'  > "$CD5/GAMES/KEEN1/KEEN.1"
# a folder the catalogue cannot possibly name, scanned AFTER the replacements
printf 'MZ' > "$CD5/ZZUNIQ/ZZUNIQ.EXE";       printf 'x' > "$CD5/ZZUNIQ/ZZUNIQ.DAT"
run_dos "$CD5" 'C:\DOSGAME\DOSGAME.EXE /selftest'
SH="$CD5/DOSGAME/DGSELF.TXT"
if grep -qi 'replaces' "$CD5/DOSGAME/DOSGAME.LOG" 2>/dev/null; then
    ok "the fixture really does make the scan replace a row"
else
    bad "the fixture no longer exercises row replacement - the test is hollow"
fi
if grep -qiE '^1\|R\|Doom\|DOOM\.EXE\|C:\\DOOM\|' "$SH" 2>/dev/null; then
    ok "C:\\DOOM keeps its own title after the row was replaced"
else
    bad "C:\\DOOM: $(grep -i 'C:.DOOM' "$SH" 2>/dev/null | tr -d '\r')"
fi
if grep -qiE '^1\|R\|rott shareware\|ROTT\.EXE\|C:\\ROTT\|' "$SH" 2>/dev/null; then
    ok "C:\\ROTT keeps its own title after the row was replaced"
else
    bad "C:\\ROTT: $(grep -i 'C:.ROTT' "$SH" 2>/dev/null | tr -d '\r')"
fi
if grep -qiE '^1\|I\|keen1 shareware\|INSTALL\.BAT\|C:\\GAMES\\KEEN1\|' "$SH" 2>/dev/null; then
    ok "a row AFTER the shuffled ones is still named correctly"
else
    bad "KEEN1: $(grep -i KEEN1 "$SH" 2>/dev/null | tr -d '\r')"
fi
# The regression itself, and the reason it matters: a stale key does not just
# fail to match, it matches the WRONG row and puts another game's name on it.
if grep -qiE '^1\|R\|ZZUNIQ\|ZZUNIQ\.EXE\|C:\\ZZUNIQ\|' "$SH" 2>/dev/null; then
    ok "a folder the catalogue cannot name keeps its folder name"
else
    bad "an unnamable folder was given another game's title: $(grep -i ZZUNIQ "$SH" 2>/dev/null | tr -d '\r')"
fi
grep -q 'char dir\[13\];' "$SRCDIR/dosgame.c" \
  && ok "the folder name travels IN the row, not in a parallel array" \
  || bad "the folder name travels IN the row, not in a parallel array"
grep -q 'static void title_begin' "$SRCDIR/dosgame.c" \
  && ok "the comparison keys are built AFTER the scan finishes moving rows" \
  || bad "the comparison keys are built after the scan finishes moving rows"

# ====================================================== TEST 16c ===========
# A set whose disks are not all present can never install, and the list should
# say so up front rather than the operator finding out by pressing Enter.
echo "== an incomplete disk set is labelled in the list =="
CD6="$WORK/shortlabel"
rm -rf "$CD6"; mkdir -p "$CD6/DOSGAME" "$CD6/GAMES/HERETIC" "$CD6/GAMES/DOOM"
cp "$WORK/DOSGAME.EXE" "$CD6/DOSGAME/DOSGAME.EXE"
printf 'gamedir=C:\\GAMES\nscan=C:\\GAMES\n' > "$CD6/DOSGAME/DOSGAME.CFG"
# short: one part against a SIZE that needs two
printf 'MZ' > "$CD6/GAMES/HERETIC/DEICE.EXE"
printf '@echo off\r\n' > "$CD6/GAMES/HERETIC/INSTALL.BAT"
head -c 8192 /dev/zero > "$CD6/GAMES/HERETIC/HTIC_V10.1"
printf 'PATH=\\HERETIC\r\nSIZE=2863638\r\nEXPSIZE=6090000\r\n' \
    > "$CD6/GAMES/HERETIC/HTIC_V10.DAT"
# complete: the parts add up, exactly as C:\GAMES\DOOM does on .243
printf 'MZ' > "$CD6/GAMES/DOOM/DEICE.EXE"
printf '@echo off\r\n' > "$CD6/GAMES/DOOM/INSTALL.BAT"
head -c 6144 /dev/zero > "$CD6/GAMES/DOOM/DOOMS_19.1"
head -c 2048 /dev/zero > "$CD6/GAMES/DOOM/DOOMS_19.2"
printf 'PATH=\\DOOMS\r\nSIZE=8192\r\nEXPSIZE=5516000\r\n' \
    > "$CD6/GAMES/DOOM/DOOMS_19.DAT"
run_dos "$CD6" 'C:\DOSGAME\DOSGAME.EXE /selftest'
SLOG="$CD6/DOSGAME/DOSGAME.LOG"
if grep -qi '"HERETIC" is INCOMPLETE' "$SLOG" 2>/dev/null; then
    ok "a short disk set is spotted by the scan, not only on Enter"
else
    bad "the scan did not spot the short disk set"
fi
if grep -qi '"DOOM" is INCOMPLETE' "$SLOG" 2>/dev/null; then
    bad "a COMPLETE two-part set was called incomplete"
else
    ok "a complete two-part set is not called incomplete"
fi
grep -q '"INCOMPLETE"' "$SRCDIR/dosgame.c" \
  && ok "the Installed tab shows INCOMPLETE in the action column" \
  || bad "the Installed tab shows INCOMPLETE in the action column"

# ======================================================= TEST 17 ===========
# Both tabs must draw the same grid and the same green. They used to disagree:
# 40-column title with no marker and no colour on one, 36-column title with a
# marker and green on the other, so tabbing shifted and recoloured everything.
echo "== both tabs share one column grid and one colour rule =="
grep -q '#define TITLE_W' "$SRCDIR/dosgame.c" \
  && ok "the title column width is one named constant" \
  || bad "the title column width is one named constant"
grep -q '#define ATTR_INSTALLED' "$SRCDIR/dosgame.c" \
  && ok "the installed-game colour is one named constant" \
  || bad "the installed-game colour is one named constant"
if grep -qE 'sprintf\(line, "  %-40\.40s' "$SRCDIR/dosgame.c"; then
    bad "the Installed tab still hardcodes its own column layout"
else
    ok "neither tab hardcodes its own column layout"
fi
if [ "$(grep -c 'TITLE_W, TITLE_W, g->title' "$SRCDIR/dosgame.c")" = "2" ]; then
    ok "both tabs format the title through the same grid"
else
    bad "the two tabs do not format the title the same way"
fi
grep -q 'here ? ATTR_INSTALLED : ATTR_AVAILABLE' "$SRCDIR/dosgame.c" \
  && ok "one colour rule decides both tabs" \
  || bad "one colour rule decides both tabs"

# Launcher OUTCOMES - which exe each directory actually resolves to - live in
# their own file. Everything above is a source grep, which cannot notice the
# logic breaking while the strings survive; an adversarial review called that
# out, so these assert the result of a real run instead.
echo "== launcher outcomes (tests/test_pick_outcomes.sh) =="
if bash "$HERE/test_pick_outcomes.sh" > "$WORK/pick.log" 2>&1; then
    n=$(grep -c '^  PASS' "$WORK/pick.log")
    pass=$((pass + n))
    echo "  PASS  $n launcher outcomes asserted against the real catalogue"
else
    grep '^  FAIL' "$WORK/pick.log" || cat "$WORK/pick.log"
    n=$(grep -c '^  FAIL' "$WORK/pick.log")
    fail=$((fail + (n > 0 ? n : 1)))
fi

echo
echo "dosgame DOS tests: $pass passed, $fail failed, $skipped skipped"
[ "$fail" -eq 0 ]
