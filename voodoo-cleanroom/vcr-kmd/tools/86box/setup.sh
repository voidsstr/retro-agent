#!/usr/bin/env bash
# setup.sh - fetch 86Box v6.0 (Linux AppImage) and its ROM set into $VCR86_DIR,
# and put the test bed's machine config next to a disk image.
#   tools/86box/setup.sh [path to an XP disk image in raw format]
# The disk: the XP build VM's disk flattened to raw, prepared for this board
# first (README.md, "The disk image"): an ACPI uniprocessor kernel + HAL
# selected in boot.ini, and the PIIX4 IDE controller in the critical device
# database.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
D=${VCR86_DIR:-$HOME/retro-vm/86box}
V=v6.0
mkdir -p "$D/vm"
cd "$D"
if [ ! -x squashfs-root/AppRun ]; then
  curl -sL -o 86Box.AppImage "https://github.com/86Box/86Box/releases/download/$V/86Box-Linux-x86_64-b9001.AppImage"
  chmod +x 86Box.AppImage
  ./86Box.AppImage --appimage-extract >/dev/null
fi
if [ ! -d roms/video/voodoo ]; then
  curl -sL -o roms.zip "https://api.github.com/repos/86Box/roms/zipball/$V"
  unzip -q roms.zip && mv 86Box-roms-* roms && rm roms.zip
fi
[ -f vm/86box.cfg ] || cp "$HERE/86box.cfg" vm/86box.cfg
if [ $# -ge 1 ]; then
  ln -sf "$(readlink -f "$1")" vm/xp.img
fi
ls -la "$D" "$D/vm"
