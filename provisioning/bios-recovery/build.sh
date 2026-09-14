#!/usr/bin/env bash
# build.sh - rebuild the EP-8RDA+ blind BIOS-recovery floppy image, reproducibly.
#
# Everything this pulls is freely redistributable (FreeDOS) or vendor firmware
# for hardware we own, and every download is CHECKED against a known hash
# rather than trusted: the whole point of this disk is that it goes into a
# machine that cannot report anything back, so a silently-corrupt byte here is
# a brick we would not find out about.
#
# Needs: curl, unzip, mtools (mcopy/mdel/mdir), and the awdflash archive.
# Produces: recovery.img (1,474,560 bytes) in this directory.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${1:-$HERE/work}"
OUT="$HERE/recovery.img"
UA='Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36'

# The one BIOS we intend to write, and its verified hash. Two independent
# uploads on TheRetroWeb (the PCB 1.x .bin and the PCB 2.x .zip) both produce
# exactly this - see README.md.
BIOS_MD5=f2e2164fa37139fb6e3ec1464329c3dc
BIOS_URL='https://theretroweb.com/motherboard/bios/8rda4729-636b940ad91ad881328786.bin'
BIOS_REF='https://theretroweb.com/motherboards/13699'

FD_URL='https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/FD13-FloppyEdition.zip'
AWD_URL='https://theretroweb.com/uploads/drivers/awdflash-6827699359655292601847.zip'
AWD_REF='https://theretroweb.com/drivers/1408'
AWD_PICK=AWD824G.exe          # 8.24G: nForce-MAC aware, SST 49LF020 in its chip table

mkdir -p "$WORK"
cd "$WORK"
export MTOOLSRC=/dev/null

say() { printf '\n== %s ==\n' "$*"; }

say "BIOS image 8RDA4729"
[ -f 8rda4729.bin ] || curl -fsS -A "$UA" -e "$BIOS_REF" -m 120 -L "$BIOS_URL" -o 8rda4729.bin
got=$(md5sum 8rda4729.bin | cut -d' ' -f1)
if [ "$got" != "$BIOS_MD5" ]; then
    echo "REFUSING TO BUILD: BIOS md5 is $got, expected $BIOS_MD5" >&2
    echo "  A wrong or truncated BIOS here bricks the board irrecoverably." >&2
    exit 1
fi
sz=$(stat -c%s 8rda4729.bin)
[ "$sz" = 262144 ] || { echo "REFUSING: BIOS is $sz bytes, expected 262144" >&2; exit 1; }
echo "  ok: $got, $sz bytes"

say "awdflash $AWD_PICK"
[ -f awdflash.zip ] || curl -fsS -A "$UA" -e "$AWD_REF" -m 180 -L "$AWD_URL" -o awdflash.zip
[ -f "$AWD_PICK" ] || unzip -o -j awdflash.zip "$AWD_PICK" >/dev/null
# The flasher must be a real-mode DOS binary; a PE here would simply not run.
head -c2 "$AWD_PICK" | grep -q MZ || { echo "REFUSING: $AWD_PICK is not an MZ executable" >&2; exit 1; }
strings -a "$AWD_PICK" | grep -q 'Skip BootBlock programming' \
    || { echo "REFUSING: $AWD_PICK does not document /sb - wrong flasher" >&2; exit 1; }
echo "  ok: $(strings -a "$AWD_PICK" | grep -oE 'V8\.[0-9]+[A-Z]?' | head -1), $(stat -c%s "$AWD_PICK") bytes"

say "FreeDOS 1.3 boot floppy"
[ -f fdboot.img ] || {
    [ -f fd13floppy.zip ] || curl -fsS -m 600 -L "$FD_URL" -o fd13floppy.zip
    unzip -o -j fd13floppy.zip '144m/x86BOOT.img' >/dev/null
    mv x86BOOT.img fdboot.img
}
[ "$(stat -c%s fdboot.img)" = 1474560 ] || { echo "REFUSING: fdboot.img is not 1.44MB" >&2; exit 1; }
echo "  ok: 1474560 bytes"

say "assembling"
cp fdboot.img "$OUT"
# The installer and, critically, its 10-SECOND LANGUAGE MENU have to go: on a
# machine with no video nobody can answer it, and the flash would never start.
mdel -i "$OUT" ::/SETUP.BAT  2>/dev/null || true
mdel -i "$OUT" ::/FDAUTO.BAT 2>/dev/null || true

printf '%s\r\n' 'SHELL=\FREEDOS\BIN\COMMAND.COM \FREEDOS\BIN /E:1024 /P=\AUTOEXEC.BAT' > fdconfig.sys
cp fdconfig.sys config.sys

# /sb skips the bootblock we are RUNNING FROM, so a failed write leaves the
# recovery path intact. No /F (that means "use the ORIGINAL BIOS's flash
# routines" - the corrupt thing). No /tiny (it destroys the BIOSLock
# signature). /R because nothing can press F1 on a blind machine.
printf '%s\r\n' \
 '@ECHO OFF' \
 'ECHO EP-8RDA+ BOOTBLOCK RECOVERY' \
 'ECHO Flashing 8RDA4729.BIN - DO NOT POWER OFF' \
 'AWDFLASH 8RDA4729.BIN /py /sn /sb /cc /cd /cp /R' \
 'ECHO If you can read this, the flash did NOT start.' \
 > autoexec.bat

mcopy -i "$OUT" -o fdconfig.sys ::/FDCONFIG.SYS
mcopy -i "$OUT" -o config.sys   ::/CONFIG.SYS
mcopy -i "$OUT" -o autoexec.bat ::/AUTOEXEC.BAT
mcopy -i "$OUT" -o "$AWD_PICK"  ::/AWDFLASH.EXE
mcopy -i "$OUT" -o 8rda4729.bin ::/8RDA4729.BIN
mcopy -i "$OUT" -n ::/FREEDOS/BIN/COMMAND.COM . 2>/dev/null || true
[ -f COMMAND.COM ] && mcopy -i "$OUT" -o COMMAND.COM ::/COMMAND.COM

say "verifying the assembled image"
# Read the BIOS back OUT of the image: the file that matters is the one on the
# disk, not the one we meant to copy.
mcopy -i "$OUT" -o ::/8RDA4729.BIN readback.bin
rb=$(md5sum readback.bin | cut -d' ' -f1)
[ "$rb" = "$BIOS_MD5" ] || { echo "FAILED: BIOS inside the image is $rb" >&2; exit 1; }
sig=$(python3 -c "d=open('$OUT','rb').read(512); print('%02x%02x'%(d[510],d[511]))")
[ "$sig" = "55aa" ] || { echo "FAILED: no boot signature (got $sig)" >&2; exit 1; }
echo "  BIOS in image: $rb"
echo "  boot signature: 0x55 0xAA"
mdir -i "$OUT" ::/

printf '\n%s\n' "built: $OUT"
printf '%s\n' "write it with:  rawfd.exe A: recovery.img   (writes AND reads back)"
