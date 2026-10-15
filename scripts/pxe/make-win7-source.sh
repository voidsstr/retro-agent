#!/usr/bin/env bash
# Build the Windows 7 Professional network install for retro-pxe - x64 for a CPU
# with long mode (iPXE `cpuid --ext 29`), x86 otherwise.
#
#   target ROM --proxyDHCP (bootfile_by_mac)--> win7/undionly.kpxe   (TFTP)
#   iPXE (embedded script) -------------------> win7/boot.ipxe       (TFTP)
#   wimboot + win7/<x64|x86>/bootmgr,BCD,boot.sdi,boot.wim  --> http://.132:8069
#   Setup's WinPE runs X:\fleet\install.cmd --> net use \\192.168.1.122\files
#   setup.exe from Files/OS/Win7PRO-<x64|x86>-PXE (install.wim stays on the NAS)
#
# Only MACs listed in pxe_config.json "bootfile_by_mac" get this; every other
# machine keeps the XP default. Setup is INTERACTIVE (no unattend, no key in
# any served file): someone picks the disk at the machine.
#
# Three things that are not obvious, all measured 2026-10-14/15:
#  - WinPE 3.0's winpeshl.exe drops the arguments on a [LaunchApps] line, so
#    "cmd.exe, /k install.cmd" opens a bare prompt. The script is started by
#    cmd's AutoRun value in the image's SOFTWARE hive instead, behind a guard.
#  - Setup's WinPE has NO RTL8168/8111 driver (netrtl32 is 8139 only), and
#    the newer inbox one in install.wim (netrtx32, 2008) stops at REV_03 -
#    .145 is REV_06 and got "No network" (2026-10-15). Realtek's signed
#    driver (x86 rt86win7 7.072 REV_02-09, x64 rt64win7 7.100 REV_01-09) comes
#    from the Microsoft Update Catalog, sha256-pinned, into X:\fleet\drivers,
#    and is drvload'ed.
#  - boot.wim (145 MB) over this server's TFTP: two of three transfers logged
#    DONE were rejected by iPXE. The payload goes over HTTP (pxe_server.py
#    HTTPServer, port 8069).
#
# Usage:  bash scripts/pxe/make-win7-source.sh            (build + stage both arches)
#         bash scripts/pxe/make-win7-source.sh --upload   (also push the DVDs to the NAS)
# The x64 half is skipped (x86 only, boot.ipxe pinned to x86) when its ISO is absent.
# Needs: 7z cabextract wimtools libhivex-bin gcc make liblzma-dev git curl smbclient
set -euo pipefail

STAGE="${STAGE:-/srv/retro-pxe/win7-stage}"
TFTP="${TFTP:-/srv/retro-pxe/tftp/win7}"
SERVER_IP="192.168.1.132"
HTTP_PORT=8069
IPXE_TAG="v2.0.0"
WIMBOOT_URL="https://github.com/ipxe/wimboot/releases/download/v2.9.0/wimboot.i386"
# arch | ISO | NAS copy | Realtek cab (Microsoft Update Catalog) | sha256 | driver base name
X86_ISO="${X86_ISO:-/mnt/retro-share/Files/OS/en_windows_7_professional_x86_dvd_x15-65804.iso}"
X64_ISO="${X64_ISO:-/mnt/retro-share/Files/OS/en_windows_7_professional_with_sp1_x64_dvd_u_676939.iso}"
# x86: Realtek PCIe GbE 7.072.0410.2013, catalog update bab093c9-7753-4fad-bf63-21915fb7c795
X86_CAB="https://catalog.s.download.windowsupdate.com/c/msdownload/update/driver/drvs/2013/11/20558488_4bb7481b1141c22746082c28c808c412e3c15f36.cab"
X86_SHA="b838559be1f4fd27f4be04167059cf3bacd17b168526ffae99be4668a8744bfb"
# x64: Realtek PCIe GbE 7.100.0422.2016, catalog update 6241bfef-f658-4b66-a9eb-55df7b945b12
X64_CAB="https://catalog.s.download.windowsupdate.com/c/msdownload/update/driver/drvs/2016/08/20906799_46944ef4419a10a910930eab412007df8669daef.cab"
X64_SHA="b478450977801f9875f4cd6a880e591295e13ed75a1198937e504d5949b3f68b"
HERE="$(cd "$(dirname "$0")" && pwd)"
UPLOAD=0
[ "${1:-}" = "--upload" ] && UPLOAD=1

mkdir -p "$STAGE/build" "$TFTP"
cd "$STAGE"

# iPXE with the hand-over script baked in (no second DHCP round with us).
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
# wimboot.i386 runs on any x86 CPU, and boots a 64-bit WinPE too (BIOS bootmgr
# is 32-bit and loads the 64-bit winload itself).
[ -f build/wimboot.i386 ] || curl -sfL -o build/wimboot.i386 "$WIMBOOT_URL"

build_arch() {   # <arch> <iso> <cab url> <cab sha256> <driver base>
    local A=$1 ISO=$2 CAB=$3 SHA=$4 DRV=$5
    local NAS_REL="Files/OS/Win7PRO-$A-PXE" W="$STAGE/$A"
    echo "== $A from $ISO"
    mkdir -p "$W" && cd "$W"
    # 1. The DVD, extracted.
    if [ ! -f dvd/sources/install.wim ]; then
        rm -rf dvd && 7z x -y -odvd "$ISO" >/dev/null
    fi
    # 2. The NAS copy Setup installs from, verified file by file through the
    #    read-only /mnt mount - a different path from the smbclient that wrote it.
    ( cd dvd && find . -type f -print0 | sort -z | xargs -0 md5sum ) > dvd.md5
    if [ "$UPLOAD" = 1 ]; then
        echo "[$A] uploading the DVD to //192.168.1.122/files/$NAS_REL"
        ( cd "$HERE/../fleet" && python3 -c "
import subprocess, sys
from sharewrite import Auth
with Auth() as a:
    r = subprocess.run(['smbclient', '//192.168.1.122/files', '-A', a, '-c',
        'mkdir $NAS_REL; cd $NAS_REL; lcd $W/dvd; recurse ON; prompt OFF; mput *'],
        capture_output=True, text=True)
    sys.exit(r.returncode)" )
    fi
    echo "[$A] verifying the NAS copy ($(wc -l < dvd.md5) files)"
    ( cd "/mnt/retro-share/$NAS_REL" && md5sum -c --quiet "$W/dvd.md5" ) \
        || { echo "$A NAS copy is missing or differs - re-run with --upload" >&2; exit 1; }
    # 3. Setup's WinPE (boot.wim index 2) + launcher, NIC driver and AutoRun.
    rm -rf peadd hive rtk && mkdir -p peadd/fleet/drivers hive rtk
    printf '[LaunchApps]\r\nX:\\Windows\\System32\\cmd.exe\r\n' > peadd/winpeshl.ini
    sed 's/$/\r/' "$HERE/win7-install.cmd" > peadd/fleet/install.cmd
    local cabf="$STAGE/build/rtl8168-win7-$A.cab"
    [ -f "$cabf" ] || curl -sfL -o "$cabf" "$CAB"
    echo "$SHA  $cabf" | sha256sum -c --quiet \
        || { echo "Realtek $A cab checksum mismatch" >&2; rm -f "$cabf"; exit 1; }
    ( cd rtk && cabextract -q "$cabf" >/dev/null )
    # The INF's [SourceDisksFiles] also names RtNicProp*.dll and RTNUninst*.dll:
    # without them drvload fails 0x80070002 (file not found) - seen in the x64
    # VM. Every payload file goes in; only the one INF (the cf/hp/ws variants
    # are other OEMs' copies of the same driver).
    cp "rtk/$DRV.inf" peadd/fleet/drivers/
    find rtk -maxdepth 1 -type f ! -iname '*.inf' ! -iname '*.txt' -exec cp {} peadd/fleet/drivers/ \;
    for f in $(sed -n '/^\[SourceDisksFiles\]/,/^\[/p' <(iconv -f utf-16 -t utf-8 "rtk/$DRV.inf" 2>/dev/null || cat "rtk/$DRV.inf") \
               | sed -n 's/^\([^;[ ][^ =]*\) *=.*/\1/p'); do
        ls peadd/fleet/drivers | grep -qix "$f" \
            || { echo "$A: $DRV.inf needs $f, not in the cab" >&2; exit 1; }
    done
    wimextract dvd/sources/boot.wim 2 /Windows/System32/config/SOFTWARE --dest-dir=hive >/dev/null 2>&1
    printf 'cd \\Microsoft\\Command Processor\nsetval 1\nAutoRun\nstring:X:\\fleet\\install.cmd\ncommit\n' \
        | hivexsh -w hive/SOFTWARE
    [ "$(hivexget hive/SOFTWARE '\Microsoft\Command Processor' AutoRun)" = 'X:\fleet\install.cmd' ]
    cp dvd/sources/boot.wim boot.wim
    printf 'add %s /Windows/System32/winpeshl.ini\nadd %s /fleet\nadd %s /Windows/System32/config/SOFTWARE\n' \
        "$W/peadd/winpeshl.ini" "$W/peadd/fleet" "$W/hive/SOFTWARE" | wimupdate boot.wim 2 >/dev/null
    # 4. Stage.
    mkdir -p "$TFTP/$A"
    cp dvd/bootmgr dvd/boot/bcd dvd/boot/boot.sdi boot.wim "$TFTP/$A/"
    cd "$STAGE"
}

build_arch x86 "$X86_ISO" "$X86_CAB" "$X86_SHA" rt86win7
PICK='cpuid --ext 29 && set a x64 || set a x86'
if [ -f "$X64_ISO" ] || [ -f "$STAGE/x64/dvd/sources/install.wim" ]; then
    build_arch x64 "$X64_ISO" "$X64_CAB" "$X64_SHA" rt64win7
else
    echo "== x64 skipped: $X64_ISO not found - every machine gets x86"
    PICK='set a x86'
fi

cp build/ipxe/src/bin/undionly.kpxe build/wimboot.i386 "$TFTP/"
cat > "$TFTP/boot.ipxe" <<EOF
#!ipxe
# Windows 7 network install (built by scripts/pxe/make-win7-source.sh).
# 64-bit Setup when the CPU has long mode (CPUID 80000001h EDX bit 29), else
# 32-bit. wimboot boots Setup's own WinPE, whose X:\\fleet\\install.cmd maps the
# NAS and runs Setup from \\\\192.168.1.122\\files\\Files\\OS\\Win7PRO-<arch>-PXE.
$PICK
set s http://$SERVER_IP:$HTTP_PORT/win7
echo Loading Windows 7 \${a} Setup over HTTP...
kernel \${s}/wimboot.i386
initrd \${s}/\${a}/bootmgr  bootmgr
initrd \${s}/\${a}/bcd      BCD
initrd \${s}/\${a}/boot.sdi boot.sdi
initrd \${s}/\${a}/boot.wim boot.wim
boot || shell
EOF
# The single-arch layout (win7/boot.wim ...) is gone; never serve a stale one.
rm -f "$TFTP/bootmgr" "$TFTP/bcd" "$TFTP/boot.sdi" "$TFTP/boot.wim"
ls -laR "$TFTP"
echo "staged. Check it: sudo python3 scripts/pxe/pxe_selftest.py --mac <a bootfile_by_mac MAC>"
