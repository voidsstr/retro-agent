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


def macs_from_hwprofile(text):
    """MACs from HWPROFILE's network.interfaces[], normalised to aa:bb:.. form.
    Empty list on anything unparsable - the caller then tries ipconfig."""
    try:
        ifaces = json.loads(text).get('network', {}).get('interfaces', [])
    except (ValueError, AttributeError):
        return []
    out = []
    for i in ifaces:
        m = str(i.get('mac') or '').strip()
        if re.fullmatch(r'[0-9A-Fa-f]{2}([-:][0-9A-Fa-f]{2}){5}', m):
            m = m.replace('-', ':').lower()
            if m != '00:00:00:00:00:00' and m not in out:
                out.append(m)
    return out


async def agent_mac(ip):
    """Ask the machine for its own MAC. More reliable than the host ARP cache,
    which can be stale or hold the address a DIFFERENT interface had.

    HWPROFILE first: it runs inside the agent, so it works on Windows 9x, which
    has no cmd.exe - the ipconfig route could never read a Win98 box's MAC, so
    this script refused to reboot every 9x machine (found on .243, 2026-09-24).
    It also spawns no child process, which on a single-threaded 9x agent is the
    difference between a safe query and a dead agent (CLAUDE.md)."""
    c = RetroConnection(ip, 9898)
    await asyncio.wait_for(c.connect(SECRET), timeout=20)
    try:
        try:
            macs = macs_from_hwprofile(await c.command_text('HWPROFILE', timeout=60))
        except Exception:  # noqa: BLE001 - an older agent without HWPROFILE
            macs = []
        if macs:
            return macs
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
        required, grace, source = wpa_from_licstatus(lic)
        if required is None:
            try:
                required, grace = parse_wmic_wpa(await c.command_text(
                    'EXEC wmic path Win32_WindowsProductActivation get /value', timeout=60))
                source = 'wmic'
            except Exception:                        # noqa: BLE001
                pass
        if required:
            return True, (f'Windows reports activation REQUIRED with {grace} grace day(s) left '
                          f'({source})' + (' - logon WILL be refused after a reboot'
                                           if grace == 0 else '') + (f'; {nag}' if nag else ''))
        if nag:
            return True, nag
        if seen.get('activation_required') == 'present':
            return True, 'Winlogon reports activation required'
        if required is None:
            return False, ('no activation nag and no activation-required flag '
                           '(Windows\' own verdict could not be read)')
        return False, f'activated ({source}), no activation nag'
    finally:
        await c.close()


def wpa_from_licstatus(lic):
    """Windows' own verdict from LICSTATUS's 'wpa' entry (agent 1.85.2+).

    Returns (required, grace_days, source); required is None when the agent is
    older or could not ask WMI. The Winlogon flag LICSTATUS also reports was
    "not present" on the 2026-09-26 Dell while WMI said grace 0 - so this, not
    the flag, is the answer when it is available."""
    for v in lic.get('values', []):
        if v.get('id') == 'wpa' and v.get('observed') in ('required', 'activated'):
            return (v.get('observed') == 'required', int(v.get('grace_days', 0)),
                    'LICSTATUS/WMI')
    return None, None, None


def parse_wmic_wpa(text):
    """(required, grace_days) from `wmic path Win32_WindowsProductActivation get
    /value`, a READ-ONLY query that works through any agent's EXEC. (None, None)
    if the output carries no answer. `get`, never `call`: the class's methods
    change activation, which is the operator's call, not a reboot tool's."""
    vals = dict(re.findall(r'^\s*(\w+)=(\S*)\s*$', text or '', re.M))
    if 'ActivationRequired' not in vals:
        return None, None
    required = vals['ActivationRequired'].strip() == '1'
    try:
        grace = int(vals.get('RemainingGracePeriod', '0') or 0)
    except ValueError:
        grace = 0
    return required, (grace if required else 0)


async def reboot(ip):
    c = RetroConnection(ip, 9898)
    await asyncio.wait_for(c.connect(SECRET), timeout=20)
    try:
        return await c.command_text('REBOOT', timeout=30)
    finally:
        await c.close()


def arp_mac(ip):
    """The host's ARP/neighbour entry. Used only by --rpc, where the agent is
    by definition not answering and cannot report its own MAC."""
    r = subprocess.run(['ip', 'neigh', 'show', ip], capture_output=True, text=True)
    m = re.search(r'lladdr\s+([0-9a-f:]{17})', r.stdout)
    return [m.group(1)] if m else []


def rpc_reboot(ip, user, password):
    """Reboot through the Windows RPC shutdown service - the route that still
    works when the AGENT is dead or the display driver has wedged user mode
    while the kernel still answers SMB (.124's V5 6000 wedge: 9898 refused,
    9897 mute, 445 up). Needs, on the box: ForceGuest=0
    (HKLM\\SYSTEM\\CurrentControlSet\\Control\\Lsa\\forceguest), and it
    speaks SMB1 + NTLMv1 because that is what XP accepts."""
    r = subprocess.run(
        ['net', 'rpc', 'shutdown', '-r', '-f', '-t', '5',
         '-C', 'safe-reboot.py --rpc (agent unreachable)',
         '-I', ip, '-U', f'{user}%{password}',
         '--option=client min protocol=NT1',
         '--option=client ntlmv2 auth=no'],
        capture_output=True, text=True, timeout=90)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


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
    ap.add_argument('--rpc', action='store_true',
                    help='the agent is not answering: take the MAC from the host '
                         'ARP table and reboot through Windows RPC (net rpc '
                         'shutdown) instead of the agent. Activation cannot be '
                         'checked without the agent, so it is reported as unknown.')
    ap.add_argument('--rpc-user', default='Administrator',
                    help='console account (fleet convention password: "password")')
    ap.add_argument('--rpc-password', default='password')
    a = ap.parse_args()

    if a.rpc:
        macs = arp_mac(a.ip)
        if not macs:
            print(f'{a.ip}: no ARP entry - cannot arm the boot hold, NOT rebooting',
                  file=sys.stderr)
            return 2
        for m in macs:
            ok, msg = hold('arm', m)
            if not ok:
                print(f'  could not arm a hold for {m} - NOT rebooting', file=sys.stderr)
                return 3
            print(f'  hold armed for {m} (from ARP)')
        print(f'  activation UNKNOWN (agent unreachable)', file=sys.stderr)
        ok, msg = rpc_reboot(a.ip, a.rpc_user, a.rpc_password)
        print(f'  rpc shutdown {a.ip}: {"ok" if ok else "FAILED"} {msg}')
        return 0 if ok else 5

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
