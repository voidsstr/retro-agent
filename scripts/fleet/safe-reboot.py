#!/usr/bin/env python3
"""Reboot a fleet machine without letting it reinstall itself.

WHY THIS EXISTS. The fleet boxes boot from the network FIRST - that is how they
get imaged in the first place. So a plain REBOOT is only safe while the PXE
server is refusing to serve that machine. On 2026-08-28 a Gateway 550 was
rebooted to verify an auto-login change, minutes after its boot hold had been
cleared as collateral damage from a `--release all`. It PXE booted, took the
offer, and repartitioned itself - losing an hour of provisioning to a command
that was supposed to be a no-op.

The agent's REBOOT cannot know any of this; it just reboots. So arming the hold
has to happen HERE, before the reboot, and the reboot must not proceed if the
hold could not be armed.

    safe-reboot.py <ip> [--reinstall]

--reinstall inverts it: release the hold and reboot, i.e. deliberately reimage.
"""
import argparse
import asyncio
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from client.retro_protocol import RetroConnection

SECRET = 'retro-agent-secret'
PXE = Path(__file__).resolve().parents[1] / 'pxe' / 'pxe_server.py'


async def agent_mac(ip):
    """Ask the machine for its own MAC. More reliable than the host ARP cache,
    which can be stale or hold the address a DIFFERENT interface had."""
    c = RetroConnection(ip, 9898)
    await asyncio.wait_for(c.connect(SECRET), timeout=20)
    try:
        out = await c.command_text('EXECW 40 cmd /c ipconfig /all', timeout=60)
    finally:
        await c.close()
    macs = re.findall(r'Physical Address[.\s]*:\s*([0-9A-Fa-f-]{17})', out)
    return [m.replace('-', ':').lower() for m in macs]


async def reboot(ip):
    c = RetroConnection(ip, 9898)
    await asyncio.wait_for(c.connect(SECRET), timeout=20)
    try:
        return await c.command_text('REBOOT', timeout=30)
    finally:
        await c.close()


def hold(action, mac):
    r = subprocess.run([sys.executable, str(PXE), f'--{action}', mac],
                       capture_output=True, text=True, timeout=60)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ip')
    ap.add_argument('--reinstall', action='store_true',
                    help='release the hold instead, so the box DOES reimage')
    a = ap.parse_args()

    macs = await agent_mac(a.ip)
    if not macs:
        print(f'{a.ip}: could not read a MAC from the machine - refusing to '
              f'reboot, because I cannot arm its boot hold', file=sys.stderr)
        return 2
    print(f'  {a.ip} MACs: {", ".join(macs)}')

    if a.reinstall:
        for m in macs:
            ok, msg = hold('release', m)
            print(f'  release {m}: {"ok" if ok else "FAILED"} {msg}')
        print('  *** this box WILL reinstall on reboot ***')
    else:
        # Arm every MAC: a box with two NICs can PXE from either.
        for m in macs:
            ok, msg = hold('arm', m)
            if not ok:
                print(f'  could not arm a hold for {m} - NOT rebooting', file=sys.stderr)
                return 3
            print(f'  hold armed for {m}')

    print(f'  rebooting {a.ip}: {await reboot(a.ip)}')
    return 0


if __name__ == '__main__':
    sys.exit(asyncio.run(main()))
