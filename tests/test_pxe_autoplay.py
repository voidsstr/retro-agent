#!/usr/bin/env python3
"""A freshly imaged box must have AutoPlay OFF on every drive type.

WHY THIS TEST EXISTS. The staged disc-gated titles - System Shock 2, Thief,
Descent 2, StarCraft, and the rest as their images land - MOUNT THEIR OWN ISO
at launch, because a 1990s CD check wants a disc in a drive and is not satisfied
by staging the disc's files into the game folder (retro-3dfx/FINDINGS.md,
2026-08-29). Mounting raises a modal AutoPlay window, on top of a game that is
starting and usually fullscreen: it steals focus, it can make the title bail
during init, and it sits in the middle of every screenshot taken to verify the
game afterwards. On a freshly imaged machine that happens on the FIRST launch of
every one of those titles.

It belongs in the image for the same reason the firewall setting does: a
hand-applied fix does not survive the next re-image, and these boxes get
re-imaged.

0xFF sets the bit for every drive type - removable, fixed, network, CD-ROM, RAM.
Anything less leaves a hole: 0x95 (the common "CD-ROM only" value) still autoplays
a mounted virtual drive on some XP builds, which is exactly the drive these games
create.

BOTH HIVES. HKLM is the machine policy. HKCU here is the Default User hive,
because cmdlines.txt merges this file at T-12 - so every profile created
afterwards inherits it. Setting only HKLM leaves the per-user policy unset and
Explorer's own default wins for the logged-on account.

Source: scripts/pxe/stage-oem.sh (the retroagent.reg block).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SCRIPT = os.path.join(REPO, 'scripts', 'pxe', 'stage-oem.sh')
STAGED = '/mnt/retro-share/Files/OS/XPSP3-FLEET/$OEM$/retroagent.reg'

# One backslash in the staged .reg, two in the shell printf that writes it.
POLICY = r'Windows\\{1,2}CurrentVersion\\{1,2}Policies\\{1,2}Explorer'
fails = []


def check(label, cond):
    print(f'  {"PASS" if cond else "FAIL"}  {label}')
    if not cond:
        fails.append(label)


def autoplay_values(text):
    """Every NoDriveTypeAutoRun dword in the file, with the key it sits under."""
    out = []
    key = None
    for line in text.split('\n'):
        line = line.strip()
        m = re.match(r'^\[(.+)\]$', line)
        if m:
            key = m.group(1)
            continue
        m = re.match(r'^"NoDriveTypeAutoRun"\s*=\s*dword:([0-9a-fA-F]+)',
                     line, re.I)
        if m:
            out.append((key or '', int(m.group(1), 16)))
    return out


def main():
    print('== the generator emits it ==')
    if not os.path.isfile(SCRIPT):
        print(f'  FAIL  {SCRIPT} not found')
        return 1
    with open(SCRIPT, encoding='latin1') as fh:
        gen = fh.read()
    check('stage-oem.sh writes NoDriveTypeAutoRun',
          'NoDriveTypeAutoRun' in gen)
    check('under HKLM ...Policies\\Explorer',
          re.search(r'HKEY_LOCAL_MACHINE.*' + POLICY, gen) is not None)
    check('and under HKCU ...Policies\\Explorer (the Default User hive at T-12)',
          re.search(r'HKEY_CURRENT_USER.*' + POLICY, gen) is not None)

    print('== the staged image carries it ==')
    if not os.path.isfile(STAGED):
        print(f'  SKIP  {STAGED} not present (share not mounted)')
        print(f'\npxe autoplay: {"generator OK" if not fails else str(len(fails)) + " FAILED"}')
        return 1 if fails else 0

    with open(STAGED, encoding='latin1') as fh:
        reg = fh.read()
    check('retroagent.reg is REGEDIT4 (ANSI - the file this is written into)',
          reg.startswith('REGEDIT4'))

    vals = autoplay_values(reg)
    check('NoDriveTypeAutoRun appears twice', len(vals) == 2)
    hives = {k.split('\\')[0].upper() for k, _v in vals}
    check('once for HKEY_LOCAL_MACHINE and once for HKEY_CURRENT_USER',
          hives == {'HKEY_LOCAL_MACHINE', 'HKEY_CURRENT_USER'})
    check('both under Policies\\Explorer',
          all(re.search(POLICY, k) for k, _v in vals))
    check('both are 0xFF - every drive type, including a mounted virtual one',
          all(v == 0xFF for _k, v in vals) and len(vals) == 2)
    # The old shape, named so it cannot creep back as "good enough".
    check('not the CD-ROM-only 0x95, which still autoplays a virtual drive',
          all(v != 0x95 for _k, v in vals))

    print(f'\npxe autoplay: {"all checks passed" if not fails else str(len(fails)) + " FAILED"}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
