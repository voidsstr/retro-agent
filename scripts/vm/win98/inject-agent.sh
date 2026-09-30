#!/usr/bin/env bash
# inject-agent.sh VMDIR - put the retro agent into a SHUT-DOWN Win98 build VM's disk.
# Copies the share's current retro_agent.exe (checked against its .ver), the
# fleet's AGENTRUN.BAT, and FIRSTRUN.BAT + VMSETUP.REG, and points WIN.INI
# run= at FIRSTRUN.BAT, which on the next logon writes the RetroAgent Run key
# and HKLM\Network\Logon AutoLogon once, then starts the agent.
# The VM must be STOPPED (writing a running guest's FAT from the host corrupts it).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/../../.." && pwd)
VMDIR=${1:-$HOME/retro-vm/86box/vm98}; I="$VMDIR/w98.img@@32256"
systemctl --user is-active --quiet w98box && { echo "REFUSED: stop the VM first (systemctl --user stop w98box)"; exit 1; }
RA="/mnt/retro-share/Utility/Retro Automation"; W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
cp "$RA/retro_agent.exe" "$W/RETRO_AGENT.EXE"; V=$(cat "$RA/retro_agent.exe.ver")
strings "$W/RETRO_AGENT.EXE" | grep -qx "$V" || { echo "the share's exe does not carry version $V"; exit 1; }
cp "$REPO/scripts/dosgames/AGENTRUN.BAT" "$W/"
for f in FIRSTRUN.BAT VMSETUP.REG; do sed 's/$/\r/' "$HERE/agent/$f" > "$W/$f"; done
mmd -i "$I" ::/RETRO_AGENT 2>/dev/null || true
for f in RETRO_AGENT.EXE AGENTRUN.BAT FIRSTRUN.BAT VMSETUP.REG; do mcopy -o -i "$I" "$W/$f" ::/RETRO_AGENT/$f; done
mtype -i "$I" ::/WINDOWS/WIN.INI > "$W/win.ini"
python3 - "$W/win.ini" <<'PY'
import re, sys
p = sys.argv[1]; b = open(p, 'rb').read()
b2 = re.sub(rb'\r\nrun=[^\r]*\r\n', b'\r\nrun=C:\\\\RETRO_AGENT\\\\FIRSTRUN.BAT\r\n', b, count=1)
assert b2 != b or b'run=C:\\RETRO_AGENT\\FIRSTRUN.BAT' in b, 'no run= line in [windows]'
open(p, 'wb').write(b2)
PY
mcopy -o -i "$I" "$W/win.ini" ::/WINDOWS/WIN.INI
# AutoScan=2: after an unclean shutdown (a hung title the sweep has to reset)
# ScanDisk fixes without asking - AutoScan=1, the default, stops for a key.
# Same length, so MSDOS.SYS keeps its required padding; its SHR attributes go
# back on.
mtype -i "$I" ::/MSDOS.SYS > "$W/msdos.sys"
python3 - "$W/msdos.sys" <<'PY'
import sys
p = sys.argv[1]; b = open(p, 'rb').read()
if b'AutoScan=2\r\n' not in b:
    assert b.count(b'AutoScan=1\r\n') == 1, 'MSDOS.SYS has no AutoScan=1 line'
    b = b.replace(b'AutoScan=1\r\n', b'AutoScan=2\r\n')
open(p, 'wb').write(b)
PY
mattrib -i "$I" -r -s -h ::/MSDOS.SYS
mcopy -o -i "$I" "$W/msdos.sys" ::/MSDOS.SYS
mattrib -i "$I" +r +s +h ::/MSDOS.SYS
echo "agent $V injected; start the VM and log on once (blank password) - later boots log on by themselves"
