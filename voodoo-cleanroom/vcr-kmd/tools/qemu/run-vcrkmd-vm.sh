#!/usr/bin/env bash
# The vcr-kmd chassis test bed: the XP build VM's disk through a THROWAWAY
# copy-on-write overlay (xp3.qcow2 itself is never written), booted with QEMU's
# std-vga (PCI 1234:1111) so our driver's Bochs backend binds, and with the
# driver's debug port captured:
#   port 0xe9 (QEMU debugcon) -> $D/debugcon.log   (Diag\DebugPort = 0xe9)
#   COM1                      -> $D/com1.log
# The guest's retro agent answers on 127.0.0.1:19910; VNC :7; monitor $D/mon.sock.
#
#   run-vcrkmd-vm.sh            (start it outside Claude Code:
#                                systemd-run --user --unit=vcrkmd-vm run-vcrkmd-vm.sh)
#   run-vcrkmd-vm.sh --fresh    discard the overlay first (back to the build VM state)
set -eu
VMDIR=${VMDIR:-/home/voidsstr/retro-vm}
D=$VMDIR/vcrkmd
mkdir -p "$D"
if [ "${1:-}" = "--fresh" ] || [ ! -f "$D/overlay.qcow2" ]; then
  rm -f "$D/overlay.qcow2"
  qemu-img create -q -f qcow2 -b "$VMDIR/xp3.qcow2" -F qcow2 "$D/overlay.qcow2"
fi
exec qemu-system-x86_64 -name vcrkmd-test \
  -enable-kvm -machine pc -cpu host -m 1024 -smp 2 -rtc base=localtime \
  -drive file=$D/overlay.qcow2,if=ide,index=0,media=disk,format=qcow2,cache=writeback \
  -usb -device usb-tablet \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:19910-:9898 \
  -device rtl8139,netdev=n0 \
  -boot order=c -vga std -vnc :7 \
  -monitor unix:$D/mon.sock,server,nowait \
  -debugcon file:$D/debugcon.log -global isa-debugcon.iobase=0xe9 \
  -serial file:$D/com1.log
