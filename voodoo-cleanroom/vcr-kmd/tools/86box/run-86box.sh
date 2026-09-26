#!/usr/bin/env bash
# The vcr-kmd Voodoo3 test bed: 86Box v6.0 emulating an ASUS P3B-F (440BX),
# a Pentium II 450, 512 MB and a 3dfx Voodoo3 3000 AGP, with the guest's retro
# agent forwarded from 0.0.0.0:19920 (SLiRP). Headless: it draws on its own
# Xvfb display (:21), which 86box-shot.py captures and 86box-key.py types into.
#
#   systemd-run --user --unit=vcr86box tools/86box/run-86box.sh
#
# $VCR86_DIR (default ~/retro-vm/86box) holds what setup.sh fetched:
# squashfs-root/ (the extracted AppImage), roms/, and vm/ (86box.cfg + xp.img).
D=${VCR86_DIR:-$HOME/retro-vm/86box}
DISP=${VCR86_DISPLAY:-:21}
Xvfb "$DISP" -screen 0 1280x1024x24 -nolisten tcp >"$D/vm/xvfb.log" 2>&1 &
XV=$!
trap 'kill $XV 2>/dev/null' EXIT
sleep 1
export DISPLAY=$DISP QT_QPA_PLATFORM=xcb
"$D/squashfs-root/AppRun" --vmpath "$D/vm" --rompath "$D/roms" --noconfirm "$@"
