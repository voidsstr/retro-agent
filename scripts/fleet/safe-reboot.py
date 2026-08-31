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


async def activation_risk(ip):
    """Would this box come back from a reboot?

    CLAUDE.md makes this REQUIRED and nothing enforced it, which is how it kept
    happening. An unactivated XP box is fine while it is logged in and
    UNREACHABLE the moment it restarts: when the grace expires Windows blocks
    logon entirely, so the console session never starts, the Run-key
    RetroAgent value never fires, and the machine comes back with networking up
    (445/139/135 open) and the agent DEAD. It looks like a failed boot; it is a
    locked activation screen, and there is no remote path back.

    That cost .171 a day on 2026-08-29 - and the box had been flagged weeks
    earlier as "not activated, wpabaln.exe runs at logon, not blocking yet".
    Nobody connected the two facts, because nothing made them meet.

    Returns (risky, why). Read-only: LICSTATUS only reports.
    """
    c = RetroConnection(ip, 9898)
    await asyncio.wait_for(c.connect(SECRET), timeout=20)
    try:
        try:
            lic = json.loads(await c.command_text('LICSTATUS', timeout=30))
        except Exception as e:                       # noqa: BLE001
            return None, f'could not read LICSTATUS ({e})'
        if not lic.get('is_winxp'):
            return False, 'not Windows XP - the XP activation lockout does not apply'
        seen = {v.get('id'): v.get('observed') for v in lic.get('values', [])}
        nag = ''
        try:
            out = await c.command_text('EXEC tasklist', timeout=40)
            if 'wpabaln' in out.lower():
                nag = 'wpabaln.exe is RUNNING (the activation nag)'
        except Exception:
            pass                                     # 9x has no tasklist; XP always does
        if nag:
            return True, nag
        if seen.get('activation_required') == 'present':
            return True, 'Winlogon reports activation required'
        return False, 'no activation nag and no activation-required flag'
    finally:
        await c.close()


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
    ap.add_argument('--ignore-activation', action='store_true',
                    help='reboot even though this box may not survive it. Only '
                         'with a keyboard in reach of the machine.')
    a = ap.parse_args()

    # ACTIVATION BEFORE ANYTHING ELSE. Arming a PXE hold protects the disk;
    # it does nothing about a box that will never reach a logon again.
    risky, why = (False, 'skipped (--ignore-activation)') if a.ignore_activation \
        else await activation_risk(a.ip)
    if risky:
        print(f'{a.ip}: REFUSING to reboot - {why}.\n'
              f'  An unactivated XP box does not come back: logon is blocked, so\n'
              f'  the agent never starts and there is no remote path in. Resolve\n'
              f'  activation first, or re-run with --ignore-activation if you are\n'
              f'  physically at the machine.', file=sys.stderr)
        return 4
    if risky is None:
        print(f'  {a.ip}: activation UNKNOWN - {why}; continuing', file=sys.stderr)
    else:
        print(f'  activation ok: {why}')

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
