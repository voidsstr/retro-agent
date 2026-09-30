#!/usr/bin/env bash
# 86Box Win98 SE build VM - the Win9x twin of ~/retro-vm/run-build.sh.
# ASUS P/I-P55T2P4 (430HX), Pentium 166 (P54C, like .243), 64 MB, Cirrus GD5436 (as .243)
# PCI + Voodoo 2 (4+4 MB), SB16 at 220/5/1/5, NE2000 PCI (RTL8029AS) on
# SLiRP: guest agent :9898 -> 127.0.0.1:19930. Own Xvfb display (:22).
#   systemd-run --user --unit=w98box -p CPUQuota=200% -p MemoryMax=2G ~/retro-vm/86box/run-98.sh
D=${W98_86BOX:-$HOME/retro-vm/86box}      # the 86Box AppImage + roms
VM=${W98_VMDIR:-$D/vm98}
DISP=${W98_DISPLAY:-:22}
Xvfb "$DISP" -screen 0 1024x768x24 -nolisten tcp >"$VM/xvfb.log" 2>&1 &
XV=$!
trap 'kill $XV 2>/dev/null' EXIT
sleep 1
export DISPLAY=$DISP QT_QPA_PLATFORM=xcb
"$D/squashfs-root/AppRun" --vmpath "$VM" --rompath "$D/roms" --noconfirm "$@"
