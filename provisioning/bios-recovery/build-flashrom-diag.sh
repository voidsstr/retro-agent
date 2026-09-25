#!/usr/bin/env bash
# build-flashrom-diag.sh - a FLASHROM diagnostic floppy for the EP-8RDA+.
#
# Why this exists: awdflash 8.24G reaches "Programming Flash Memory" and HANGS,
# both with and without /sb.  awdflash tells you nothing when it fails - it has
# no verbose mode and no log - so the next step is a flasher that reports.
#
# Booting it writes NOTHING: it probes the chipset and the flash part twice and
# logs everything to files ON THE FLOPPY, so the answer comes back even from a
# machine with no video.  Read the disk afterwards with
# scripts/fleet/rawfloppy/rawfloppy.exe on a box with a floppy drive (.184).
#
# The WRITE is on the same disk but is never automatic - a human who has read
# the probe result on screen types FLASH.  That is deliberate: the flash image
# rides along so the whole job can finish in one trip to the machine, while the
# decision to program still belongs to somebody who saw the chip get
# identified.  No chip found means FLASH cannot help and must not be tried.
#
# No backup is read off the chip.  It would cost 256 KB of floppy that the
# image needs, and build.sh already reasons out why a backup of a BIOS we
# believe to be corrupt is worthless (that is what awdflash's /sn was for).
#
# Produces: frdiag.img (1,474,560 bytes)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$HERE/work"
OUT="$HERE/frdiag.img"
export MTOOLSRC=/dev/null

FR_URL='https://www.baker76.com/download/bios/flashrom.zip'
FR_REF='https://www.baker76.com/2021/02/04/tool-flashrom-v1-2-dos/'

mkdir -p "$WORK"; cd "$WORK"
[ -f fdboot.img ] || { echo "run build.sh first (needs work/fdboot.img)" >&2; exit 1; }

[ -f flashrom-dos.zip ] || curl -fsS -A 'Mozilla/5.0' -e "$FR_REF" -L "$FR_URL" -o flashrom-dos.zip
[ -f flashromdos/flashrom.exe ] || unzip -o flashrom-dos.zip -d flashromdos >/dev/null
# It must be a DOS binary that knows this chipset and this flash part, or the
# whole trip to the machine is wasted.
head -c2 flashromdos/flashrom.exe | grep -q MZ || { echo "REFUSING: flashrom.exe is not MZ" >&2; exit 1; }
# strings | grep -q trips pipefail via SIGPIPE, so dump the strings once.
strings -a flashromdos/flashrom.exe > flashrom.strings
for want in NForce2 SST49LF020 '\-\-output'; do
    grep -q -- "$want" flashrom.strings \
        || { echo "REFUSING: flashrom.exe does not mention $want" >&2; exit 1; }
done
[ -f flashromdos/CWSDPMI.EXE ] || { echo "REFUSING: no CWSDPMI.EXE - a DJGPP binary cannot run without it" >&2; exit 1; }

cp fdboot.img "$OUT"
# The installer and its 10-second language menu go, as in build.sh.
for f in SETUP.BAT FDAUTO.BAT AUTOEXEC.BAT FDCONFIG.SYS CONFIG.SYS; do
    mdel -i "$OUT" ::/$f 2>/dev/null || true
done
# flashrom is 831 KB and the chip dump is another 256 KB, so FreeDOS keeps only
# its shell.  Everything deleted here is a utility this disk never runs.
mcopy -i "$OUT" -n ::/FREEDOS/BIN/COMMAND.COM ./COMMAND.COM
mdeltree -i "$OUT" ::/FREEDOS >/dev/null 2>&1 || true
mmd -i "$OUT" ::/FREEDOS ::/FREEDOS/BIN
mcopy -i "$OUT" -o ./COMMAND.COM ::/FREEDOS/BIN/COMMAND.COM

printf '%s\r\n' 'SHELL=\FREEDOS\BIN\COMMAND.COM \FREEDOS\BIN /E:1024 /P=\AUTOEXEC.BAT' > fdconfig.sys
cp fdconfig.sys config.sys

# No 2>&1 - FreeDOS COMMAND.COM has no stderr redirection.  flashrom's own
# -o writes the complete verbose log, which is the reason this tool was chosen.
cat > autoexec.bat <<'BAT'
@ECHO OFF
A:
CD \
SET PATH=A:\;A:\FREEDOS\BIN
ECHO ==========================================================
ECHO  EP-8RDA+ FLASH DIAGNOSTIC - THIS STEP WRITES NOTHING
ECHO  Two probes.  The second sweeps every chip flashrom
ECHO  knows and takes a few MINUTES.  Wait for DONE.
ECHO ==========================================================
ECHO.
ECHO [1/2] probe ...
FLASHROM -p internal -V -o A:\FRPROBE.TXT
ECHO.
ECHO [2/2] probe again, all buses forced on - this is the slow one ...
FLASHROM -p internal:laptop=this_is_not_a_laptop -V -o A:\FRPROBE2.TXT
ECHO.
ECHO ==========================================================
ECHO  DONE.  Read the lines above:
ECHO.
ECHO   "Found chipset" + "Found ... flash chip"  = GOOD.
ECHO      To program the BIOS now, type:   FLASH
ECHO.
ECHO   "No EEPROM/flash device found"           = STOP.
ECHO      FLASH cannot work.  Power off, bring
ECHO      the floppy back, we read the logs.
ECHO ==========================================================
BAT

# The same verified image build.sh writes - identical md5, checked again here.
[ -f 8rda4729.bin ] || { echo "run build.sh first (needs work/8rda4729.bin)" >&2; exit 1; }
got=$(md5sum 8rda4729.bin | cut -d' ' -f1)
[ "$got" = f2e2164fa37139fb6e3ec1464329c3dc ] \
    || { echo "REFUSING: BIOS md5 is $got" >&2; exit 1; }

cat > flash.bat <<'BAT'
@ECHO OFF
ECHO ==========================================================
ECHO  PROGRAMMING 8RDA4729.BIN - DO NOT POWER OFF
ECHO  flashrom erases and writes, then VERIFIES what it wrote.
ECHO ==========================================================
FLASHROM -p internal:laptop=this_is_not_a_laptop -V -w A:\8RDA4729.BIN -o A:\FRWRITE.TXT
ECHO.
ECHO ==========================================================
ECHO  Read the last lines above.
ECHO   "VERIFIED" / "Verifying flash... VERIFIED."  = written OK.
ECHO       Power off, unplug, clear CMOS, power on.
ECHO   anything else                               = DO NOT POWER OFF.
ECHO       The log is on the floppy as FRWRITE.TXT.
ECHO ==========================================================
BAT

mcopy -i "$OUT" -o flashromdos/flashrom.exe ::/FLASHROM.EXE
mcopy -i "$OUT" -o 8rda4729.bin ::/8RDA4729.BIN
mcopy -i "$OUT" -o flash.bat     ::/FLASH.BAT
mcopy -i "$OUT" -o flashromdos/CWSDPMI.EXE  ::/CWSDPMI.EXE
mcopy -i "$OUT" -o fdconfig.sys ::/FDCONFIG.SYS
mcopy -i "$OUT" -o config.sys   ::/CONFIG.SYS
mcopy -i "$OUT" -o autoexec.bat ::/AUTOEXEC.BAT

[ "$(stat -c%s "$OUT")" = 1474560 ] || { echo "REFUSING: image is not 1.44MB" >&2; exit 1; }
mdir -i "$OUT" ::/
free=$(mdir -i "$OUT" ::/ | grep -oE '[0-9 ]+ bytes free' | tr -d ' a-z')
# The three logs are written ON this disk while it is the boot disk; a full
# floppy would lose exactly the evidence the trip is for.
[ "${free:-0}" -ge 40000 ] || { echo "REFUSING: only $free bytes free, need >=40000 for the logs" >&2; exit 1; }
echo "ok: $OUT ($free bytes free for the logs)"
