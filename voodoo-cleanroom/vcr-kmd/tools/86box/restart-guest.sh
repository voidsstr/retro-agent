#!/usr/bin/env bash
# restart-guest.sh - reboot the 86Box test bed's XP guest cleanly: shut Windows
# down through its agent, wait for the agent to go, then restart the emulator
# (a cold start: 86box.cfg is re-read and no BIOS state survives). This is the
# test bed's --reboot-cmd:
#   tools/deploy_box.py install 127.0.0.1 --port 19920 --hwid 'PCI\VEN_121A&DEV_0005' \
#       --rollback-dir "" --reboot-cmd tools/86box/restart-guest.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
PORT=${VCR86_PORT:-19920}
python3 - "$PORT" "$REPO" <<'PY'
import asyncio, sys, time
sys.path.insert(0, sys.argv[2])
from client.retro_protocol import RetroConnection
port = int(sys.argv[1])
async def alive():
    try:
        c = RetroConnection('127.0.0.1', port); await c.connect('retro-agent-secret', timeout=8)
        st, d = await c.send_command('PING', timeout=8); await c.close(); return d == b'PONG'
    except Exception:
        return False
async def main():
    try:
        c = RetroConnection('127.0.0.1', port); await c.connect('retro-agent-secret', timeout=10)
        await c.send_command('EXEC cmd /c shutdown -s -f -t 2', timeout=20); await c.close()
    except Exception as e:
        print('shutdown request failed:', e)
    t0 = time.time()
    while time.time() - t0 < 120 and await alive():
        await asyncio.sleep(3)
    await asyncio.sleep(20)          # Windows finishing its shutdown after the agent went
    print(f'guest down after {time.time() - t0:.0f}s')
asyncio.run(main())
PY
systemctl --user stop vcr86box 2>/dev/null
systemctl --user reset-failed vcr86box 2>/dev/null
systemd-run --user --unit=vcr86box "$HERE/run-86box.sh" >/dev/null && echo "86Box restarted"
