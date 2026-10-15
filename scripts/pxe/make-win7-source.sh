#!/usr/bin/env bash
# Build the Windows 7 Professional x86 network install for retro-pxe.
#
#   target ROM --proxyDHCP (bootfile_by_mac)--> win7/undionly.kpxe   (TFTP)
#   iPXE (embedded script) -------------------> win7/boot.ipxe       (TFTP)
#   wimboot + bootmgr/BCD/boot.sdi/boot.wim --> http://.132:8069/win7/...
#   Setup's WinPE runs X:\fleet\install.cmd --> net use \\192.168.1.122\files
#   setup.exe from Files/OS/Win7PRO-x86-PXE (install.wim, 2 GB, stays on the NAS)
#
# Only MACs listed in pxe_config.json "bootfile_by_mac" get this; every other
# machine keeps the XP default. Setup is INTERACTIVE (no unattend, no key in
# any served file): someone picks the disk at the machine.
#
# Three things that are not obvious, all measured 2026-10-14/15:
#  - WinPE 3.0's winpeshl.exe drops the arguments on a [LaunchApps] line, so
#    "cmd.exe, /k install.cmd" opens a bare prompt. The script is started by
#    cmd's AutoRun value in the image's SOFTWARE hive instead, behind a guard.
#  - Setup's WinPE has NO RTL8168/8111 driver (netrtl32 is 8139 only). Both
#    Win7 targets have one. netrtx32 (Rt86win7.sys) is lifted from install.wim
#    into X:\fleet\drivers and drvload'ed.
#  - boot.wim (145 MB) over this server's TFTP: two of three transfers logged
#    DONE were rejected by iPXE. The payload goes over HTTP (pxe_server.py
#    HTTPServer, port 8069).
#
# Usage:  bash scripts/pxe/make-win7-source.sh            (build + stage)
#         bash scripts/pxe/make-win7-source.sh --upload   (also push the DVD to the NAS)
# Needs: 7z wimtools libhivex-bin gcc make liblzma-dev git curl smbclient
set -euo pipefail

ISO="/mnt/retro-share/Files/OS/en_windows_7_professional_x86_dvd_x15-65804.iso"
NAS_REL="Files/OS/Win7PRO-x86-PXE"
NAS_MNT="/mnt/retro-share/$NAS_REL"
STAGE="${STAGE:-/srv/retro-pxe/win7-stage}"
TFTP="${TFTP:-/srv/retro-pxe/tftp/win7}"
SERVER_IP="192.168.1.132"
HTTP_PORT=8069
IPXE_TAG="v2.0.0"
WIMBOOT_URL="https://github.com/ipxe/wimboot/releases/download/v2.9.0/wimboot.i386"
HERE="$(cd "$(dirname "$0")" && pwd)"
UPLOAD=0
[ "${1:-}" = "--upload" ] && UPLOAD=1

mkdir -p "$STAGE" "$TFTP"
cd "$STAGE"

# 1. The DVD, extracted.
if [ ! -f dvd/sources/install.wim ]; then
    echo "[1] extracting $ISO"
    rm -rf dvd && 7z x -y -odvd "$ISO" >/dev/null
fi
grep -q Professional dvd/sources/ei.cfg

# 2. The NAS copy Setup installs from. Verified file by file through the
#    read-only /mnt mount - a different path from the smbclient that wrote it.
( cd dvd && find . -type f -print0 | sort -z | xargs -0 md5sum ) > dvd.md5
if [ "$UPLOAD" = 1 ]; then
    echo "[2] uploading the DVD to //192.168.1.122/files/$NAS_REL"
    ( cd "$HERE/../fleet" && python3 -c "
import subprocess, sys
from sharewrite import Auth
with Auth() as a:
    r = subprocess.run(['smbclient', '//192.168.1.122/files', '-A', a, '-c',
        'mkdir $NAS_REL; cd $NAS_REL; lcd $STAGE/dvd; recurse ON; prompt OFF; mput *'],
        capture_output=True, text=True)
    sys.exit(r.returncode)" )
fi
echo "[2] verifying the NAS copy ($(wc -l < dvd.md5) files)"
( cd "$NAS_MNT" && md5sum -c --quiet "$STAGE/dvd.md5" ) \
    || { echo "NAS copy is missing or differs - re-run with --upload" >&2; exit 1; }

# 3. iPXE with the hand-over script baked in (no second DHCP round with us).
mkdir -p build
cat > build/embed.ipxe <<EOF
#!ipxe
# Baked into win7/undionly.kpxe. Gets a lease (the router's - the fleet proxy
# answers too and that is harmless), then hands over to the editable script.
echo NSC Retro Fleet PXE - Windows 7 network install
:retry
dhcp || goto retry
chain tftp://$SERVER_IP/win7/boot.ipxe || shell
EOF
[ -d build/ipxe ] || git clone -q https://github.com/ipxe/ipxe.git build/ipxe
( cd build/ipxe && git fetch -q --tags && git checkout -q "$IPXE_TAG" && cd src \
  && make -j"$(nproc)" bin/undionly.kpxe EMBED="$STAGE/build/embed.ipxe" > "$STAGE/build/make.log" 2>&1 ) \
    || { tail -5 build/make.log >&2; exit 1; }

# 4. wimboot (the i386 build: it also runs on 64-bit CPUs, the x86_64 one does not run on 32-bit).
[ -f build/wimboot.i386 ] || curl -sfL -o build/wimboot.i386 "$WIMBOOT_URL"

# 5. Setup's WinPE (boot.wim index 2) + the fleet launcher, driver and AutoRun.
rm -rf peadd hive rtx && mkdir -p peadd/fleet/drivers hive rtx
printf '[LaunchApps]\r\nX:\\Windows\\System32\\cmd.exe\r\n' > peadd/winpeshl.ini
sed 's/$/\r/' "$HERE/win7-install.cmd" > peadd/fleet/install.cmd
# awk reads the whole listing: grep -m1 closes the pipe early, wimdir dies of
# SIGPIPE and pipefail fails the build with no message (exit 141).
RTX=$(wimdir dvd/sources/install.wim 4 | awk '/FileRepository\/netrtx32\.inf_x86_neutral_[0-9a-f]+$/ && !f {print; f=1}')
[ -n "$RTX" ] || { echo "netrtx32 not found in install.wim" >&2; exit 1; }
wimextract dvd/sources/install.wim 4 "$RTX" --dest-dir=rtx --no-acls >/dev/null 2>&1
cp rtx/netrtx32.inf_x86_neutral_*/{netrtx32.inf,Rt86win7.sys} peadd/fleet/drivers/
wimextract dvd/sources/boot.wim 2 /Windows/System32/config/SOFTWARE --dest-dir=hive >/dev/null 2>&1
printf 'cd \\Microsoft\\Command Processor\nsetval 1\nAutoRun\nstring:X:\\fleet\\install.cmd\ncommit\n' \
    | hivexsh -w hive/SOFTWARE
[ "$(hivexget hive/SOFTWARE '\Microsoft\Command Processor' AutoRun)" = 'X:\fleet\install.cmd' ]
cp dvd/sources/boot.wim boot.wim
printf 'add %s /Windows/System32/winpeshl.ini\nadd %s /fleet\nadd %s /Windows/System32/config/SOFTWARE\n' \
    "$STAGE/peadd/winpeshl.ini" "$STAGE/peadd/fleet" "$STAGE/hive/SOFTWARE" | wimupdate boot.wim 2 >/dev/null

# 6. Stage.
cp build/ipxe/src/bin/undionly.kpxe build/wimboot.i386 dvd/bootmgr dvd/boot/bcd dvd/boot/boot.sdi boot.wim "$TFTP/"
cat > "$TFTP/boot.ipxe" <<EOF
#!ipxe
# Windows 7 network install (built by scripts/pxe/make-win7-source.sh):
# wimboot boots Setup's own WinPE, whose X:\\fleet\\install.cmd maps the NAS and
# runs Setup from \\\\192.168.1.122\\files\\Files\\OS\\Win7PRO-x86-PXE.
set s http://$SERVER_IP:$HTTP_PORT/win7
echo Loading Windows 7 Setup (about 145 MB over HTTP)...
kernel \${s}/wimboot.i386
initrd \${s}/bootmgr  bootmgr
initrd \${s}/bcd      BCD
initrd \${s}/boot.sdi boot.sdi
initrd \${s}/boot.wim boot.wim
boot || shell
EOF
ls -la "$TFTP"
echo "staged. Check it: sudo python3 scripts/pxe/pxe_selftest.py --mac <a bootfile_by_mac MAC>"
