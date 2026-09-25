#!/usr/bin/env python3
"""Keep an unattended box reachable: reboot it over Windows RPC when its agent
has been silent for too long while its kernel still answers SMB.

WHY. Driver experiments on .124 (Voodoo 5 6000) can wedge the display driver:
the agent dies or hangs (9898 refused, 9897 mute) while the kernel still
serves SMB. Until now the only cure was a person at the power switch. This
loop turns that case into a reboot with no one present:

    every --interval s   protocol PING the agent (a TCP connect proves nothing,
                         CLAUDE.md "A successful TCP connect is not liveness")
    silent for --grace s AND 445 still open
                      -> scripts/fleet/safe-reboot.py <ip> --rpc
                         (arms the PXE hold from the ARP table, then
                          net rpc shutdown -r -f)
    then back off --cooldown s, so a slow boot is never rebooted again.

It does not touch a box whose SMB is also down (the machine is off or dead -
nothing remote can help), and it never acts on a box that is answering.

    box-guardian.py 192.168.1.124 [--grace 360] [--interval 60] [--log FILE]

Start it OUTSIDE the Claude Code session, or it dies with the session:
    systemd-run --user --unit=box-guardian-124 --same-dir \
        python3 scripts/fleet/box-guardian.py 192.168.1.124 --log <file>
"""
import argparse
import asyncio
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from client.retro_protocol import RetroConnection

SAFE_REBOOT = Path(__file__).resolve().parent / 'safe-reboot.py'


def log(f, msg):
    line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}"
    print(line, flush=True)
    if f:
        with open(f, 'a') as fh:
            fh.write(line + '\n')


async def agent_alive(ip):
    try:
        c = RetroConnection(ip, 9898)
        await asyncio.wait_for(c.connect('retro-agent-secret'), timeout=20)
        try:
            st, d = await c.send_command('PING', timeout=20)
            return b'PONG' in d
        finally:
            await c.close()
    except Exception:
        return False


def smb_open(ip):
    try:
        with socket.create_connection((ip, 445), timeout=5):
            return True
    except OSError:
        return False


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ip')
    ap.add_argument('--grace', type=int, default=360)
    ap.add_argument('--interval', type=int, default=60)
    ap.add_argument('--cooldown', type=int, default=600)
    ap.add_argument('--log')
    ap.add_argument('--heartbeat', type=int, default=1800,
                    help='seconds between "agent ok" log lines (0 = every poll)')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    last_ok = time.time()
    last_beat = 0.0
    log(a.log, f'guarding {a.ip}: grace {a.grace}s, interval {a.interval}s')
    while True:
        if await agent_alive(a.ip):
            if time.time() - last_ok > 2 * a.interval:
                log(a.log, 'agent answering again')
            last_ok = time.time()
            # A guardian that only writes on trouble dies silently: twice on
            # 2026-09-24 it was killed with the Claude Code session that
            # started it and the log simply stopped. A heartbeat makes a dead
            # guardian visible in its own log. Run it under systemd-run --user.
            if time.time() - last_beat >= a.heartbeat:
                log(a.log, 'heartbeat: agent ok')
                last_beat = time.time()
        else:
            silent = time.time() - last_ok
            smb = smb_open(a.ip)
            log(a.log, f'agent silent {silent:.0f}s, smb {"up" if smb else "down"}')
            if silent >= a.grace and smb:
                log(a.log, 'WEDGE SIGNATURE - rebooting over RPC')
                if not a.dry_run:
                    r = subprocess.run([sys.executable, str(SAFE_REBOOT), a.ip, '--rpc'],
                                       capture_output=True, text=True, timeout=180)
                    log(a.log, (r.stdout + r.stderr).strip().replace('\n', ' | '))
                await asyncio.sleep(a.cooldown)
                last_ok = time.time()
                continue
        await asyncio.sleep(a.interval)


if __name__ == '__main__':
    asyncio.run(main())
