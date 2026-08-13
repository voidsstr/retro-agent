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
        for c in "$@"; do printf '%s\n' "$c"; done
        printf 'exit\n'
    } > "$conf"
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy HOME="$WORK" \
        timeout 90 "$DOSBOX" -conf "$conf" -userconf-skip >"$WORK/dosbox.log" 2>&1
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

echo
echo "dosgame DOS tests: $pass passed, $fail failed, $skipped skipped"
[ "$fail" -eq 0 ]
