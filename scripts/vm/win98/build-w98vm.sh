#!/usr/bin/env bash
# build-w98vm.sh - build the Windows 98 SE build VM (86Box) from scratch.
#
# The Win9x twin of the XP build VM (~/retro-vm/run-build.sh): a Pentium 166 +
# SB16 (220/5/1/5) + Voodoo 2 + Cirrus GD5436 machine shaped like .243, with the
# retro agent inside on 127.0.0.1:19930. Everything below is done on the HOST
# into the disk image - no keyboard work except the steps listed at the end.
#
#   bash scripts/vm/win98/build-w98vm.sh [VMDIR] [ISO]
#     VMDIR  default ~/retro-vm/86box/vm98   (needs the 86Box AppImage one level up)
#     ISO    default /mnt/retro-share/DiskImages/Windows 98 SE/faXcooL_win_98_se_bootable.iso
#
# Measured 2026-09-30 (first build):
#  - The ISO's El Torito entry is the Win98 SE startup floppy (1.44 MB, IO.SYS,
#    FDISK, HIMEM, EBD.CAB). JO.SYS is deleted from our copy (it is the CD's
#    "boot from hard disk" chooser, whose default is the empty disk).
#  - The P55T2P4 BIOS boots C: before A:, so the fresh disk gets flopmbr.S: an
#    MBR that boots the floppy. Stage 1 runs FDISK /MBR, which replaces it with
#    the standard MBR - every later boot is the hard disk, floppy or not.
#  - MSBATCH.INF (Display=0, ShowEula=0) still stops at User Information, the
#    licence and the product-key page; all three are prefilled - Enter / Alt+A
#    / Enter. The key comes from the vault at build time (fleet-win98se-product-key,
#    verified 2026-09-30 by this setup) and lives only inside the disk image.
#  - 86Box's S3 Trio64 went BLACK under Win98's own S3 Trio32/64 driver (the
#    boot log shows it loaded fine) - the VM uses a Cirrus GD5436, as .243 does.
#  - Win98 auto-logon = HKLM\Network\Logon AutoLogon=1 + a BLANK password
#    (docs/fleet-auto-login.md, Win9x section). FIRSTRUN.BAT (WIN.INI run=)
#    writes it and the RetroAgent Run key once; AGENTRUN.BAT is the fleet's own.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
VMDIR=${1:-$HOME/retro-vm/86box/vm98}
ISO=${2:-/mnt/retro-share/DiskImages/Windows 98 SE/faXcooL_win_98_se_bootable.iso}
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
IMG=$VMDIR/w98.img; OFF=$((63*512)); I="$IMG@@$OFF"
[ -e "$IMG" ] && { echo "REFUSED: $IMG exists - move it away first"; exit 1; }
mkdir -p "$VMDIR"
for t in sfdisk mkfs.fat mcopy mtype 7z cabextract as ld python3; do command -v $t >/dev/null || { echo "missing $t"; exit 1; }; done

echo "== disk: 16383/16/63 (8.4 GB), one active FAT32-LBA partition at 63"
truncate -s $((63*16*16383*512)) "$IMG"
printf 'label: dos\nstart=63, type=c, bootable\n' | sfdisk -q "$IMG"
N=$(( 63*16*16383 - 63 ))
mkfs.fat -F 32 -S 512 -s 8 -R 32 -h 63 -g 255/63 -M 0xF8 -D 0x80 -n W98BUILD --offset=63 "$IMG" $((N/2)) >/dev/null
for s in 0 6; do printf 'MSWIN4.1' | dd of="$IMG" bs=1 seek=$(((63+s)*512+3)) conv=notrunc status=none; done
as --32 -o "$WORK/flopmbr.o" "$HERE/flopmbr.S"
ld -m elf_i386 -Ttext 0x600 --oformat binary -o "$WORK/flopmbr.bin" "$WORK/flopmbr.o" 2>/dev/null
[ "$(stat -c %s "$WORK/flopmbr.bin")" = 440 ] || { echo "flopmbr must be 440 bytes"; exit 1; }
dd if="$WORK/flopmbr.bin" of="$IMG" bs=440 count=1 conv=notrunc status=none

echo "== \\WIN98 from the ISO, verified by byte total"
7z x -y -o"$WORK/iso" "$ISO" -ir'!win98/*' >/dev/null
SRC=$(du -sb "$WORK/iso/win98" | cut -f1)
mmd -i "$I" ::/WIN98
( cd "$WORK/iso/win98" && mcopy -i "$I" -s -Q ./* ::/WIN98/ )
# mtools groups digits with spaces: "864 files   181 541 424 bytes"
GOT=$(mdir -i "$I" -/ -a ::/WIN98 | sed -n 's/.* files *\([0-9 ]*\) bytes.*/\1/p' | tail -1 | tr -d ' ')
[ "$SRC" = "$GOT" ] || { echo "COPY SHORT: source $SRC bytes, image $GOT"; exit 1; }
echo "   $GOT bytes"

echo "== MSBATCH.INF (key from the vault; never printed, temp shredded)"
( cd "$REPO" && umask 077 && python3 - "$WORK/msbatch.inf" <<'PY'
import subprocess, sys
key = subprocess.run(['python3', 'scripts/fleet/keyvault.py', 'get', 'fleet-win98se-product-key'],
                     capture_output=True, text=True, check=True).stdout.strip()
assert len(key) == 29, 'the vault did not return a 5x5 key'
tpl = open('scripts/vm/win98/msbatch.inf.in').read()
open(sys.argv[1], 'w', newline='\r\n').write(tpl.replace('@PRODUCTKEY@', key))
PY
)
mcopy -i "$I" "$WORK/msbatch.inf" ::/WIN98/MSBATCH.INF; shred -u "$WORK/msbatch.inf"

echo "== setup floppy from the ISO's El Torito image"
python3 - "$ISO" "$VMDIR/setup98.img" <<'PY'
import struct, sys
S = 2048; f = open(sys.argv[1], 'rb')
f.seek(17 * S); br = f.read(S)
assert br[1:6] == b'CD001' and br[7:30] == b'EL TORITO SPECIFICATION', 'no El Torito record'
f.seek(struct.unpack('<I', br[71:75])[0] * S); e = f.read(S)[32:64]
assert e[0] == 0x88 and e[1] == 2, 'boot entry is not a bootable 1.44 MB floppy'
f.seek(struct.unpack('<I', e[8:12])[0] * S); open(sys.argv[2], 'wb').write(f.read(1474560))
PY
F=$VMDIR/setup98.img
mtype -i "$F" ::EBD.CAB > "$WORK/ebd.cab"; (cd "$WORK" && cabextract -q -F SYS.COM ebd.cab)
mdel -i "$F" ::JO.SYS
mcopy -o -i "$F" "$WORK/SYS.COM" ::SYS.COM
sed 's/$/\r/' "$HERE/setup-config.sys" > "$WORK/c.sys"; mcopy -o -i "$F" "$WORK/c.sys" ::CONFIG.SYS
sed 's/$/\r/' "$HERE/setup-autoexec.bat" > "$WORK/a.bat"; mcopy -o -i "$F" "$WORK/a.bat" ::AUTOEXEC.BAT

echo "== 86Box config"
sed "s#@ISO@#$ISO#" "$HERE/86box.cfg.in" > "$VMDIR/86box.cfg"
printf 'fdd_01_type = 35_2hd\nfdd_01_fn = setup98.img\n' >> "$VMDIR/86box.cfg"
cat <<EOF

BUILT $VMDIR. Next (see scripts/vm/win98/README.md):
  1. systemd-run --user --unit=w98box -p CPUQuota=200% -p MemoryMax=2G $HERE/run-98.sh
     (86box-key.py F1 once at "CMOS checksum error" on a new NVRAM)
  2. Setup runs from C:\\WIN98\\MSBATCH.INF; answer User Information (Return),
     licence (Alt_L+a, Return), key page (Return) - all prefilled.
  3. At the first desktop: remove fdd_01_fn from 86box.cfg, shut down (Start,
     Shut down - Stand by is PRESELECTED, pick Shut down), then
     bash $HERE/inject-agent.sh $VMDIR
EOF
