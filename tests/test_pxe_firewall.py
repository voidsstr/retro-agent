#!/usr/bin/env python3
"""A freshly imaged box must come up with the XP firewall OFF, not merely with
port 9898 opened.

WHY THIS TEST EXISTS. The image used to open a single hole for the agent
(9898/TCP) and leave the firewall running. That was enough to make the box
manageable, so it looked correct for a long time - and it is wrong for the thing
these machines exist to do.

Two failures, neither of which the 9898 rule addresses:

  1. FIRST-RUN MODAL. Every networked game binds a socket, so the first launch
     of Quake, Half-Life, Counter-Strike, UT, RA2, StarCraft and the rest raises
     a "Windows Security Alert" dialog ON TOP of the game. On a fullscreen title
     that steals focus and can make the game bail during init, which reads as a
     broken game rather than a firewall prompt - and it also sits in the middle
     of any screenshot taken to verify the title.

  2. LAN MULTIPLAYER STILL BLOCKED. Dismissing the dialog does not open the
     game's ports. Disabling only the *notifications* would hide the symptom
     while keeping the fault, which is worse than either extreme.

So the image now sets EnableFirewall=0 on both profiles. This test pins that,
and deliberately checks BOTH ways: the disable must be present, and the old
"port 9898 only, firewall still up" shape must not come back on its own.

The explicit 9898 rule is KEPT and is also asserted - it costs nothing with the
firewall off and it means the agent stays reachable if anyone ever turns the
firewall back on. Losing it would make that recovery path silently disappear.

Source: scripts/pxe/stage-oem.sh (the retroagent.reg heredoc).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SCRIPT = os.path.join(REPO, 'scripts', 'pxe', 'stage-oem.sh')
STAGED = '/mnt/retro-share/Files/OS/XPSP3-FLEET/$OEM$/retroagent.reg'

fails = []


def check(label, cond):
    print(f'  {"PASS" if cond else "FAIL"}  {label}')
    if not cond:
        fails.append(label)


def assert_firewall_off(text, where):
    """Both profiles disabled, quiet, and the 9898 rule still present."""
    for profile in ('StandardProfile', 'DomainProfile'):
        # The key stanza and its EnableFirewall must belong to the same profile,
        # so match the section header followed by the value before the next [.
        m = re.search(
            r'\[[^\]]*FirewallPolicy\\+%s\]((?:(?!\[)[\s\S])*)' % profile,
            text)
        check(f'{where}: {profile} section is present', m is not None)
        if not m:
            continue
        body = m.group(1)
        check(f'{where}: {profile} EnableFirewall=0',
              re.search(r'"EnableFirewall"\s*=\s*dword:0*0\b', body) is not None)
        check(f'{where}: {profile} DoNotAllowExceptions=0',
              re.search(r'"DoNotAllowExceptions"\s*=\s*dword:0*0\b', body) is not None)
        check(f'{where}: {profile} notifications silenced',
              re.search(r'"DisableNotifications"\s*=\s*dword:0*1\b', body) is not None)

    # The old shape must not be the ONLY thing present.
    check(f'{where}: the agent port rule is still there',
          '"9898:TCP"' in text)
    check(f'{where}: firewall is not left enabled anywhere',
          re.search(r'"EnableFirewall"\s*=\s*dword:0*1\b', text) is None)


def main():
    print('== the generator emits a firewall-off retroagent.reg ==')
    if not os.path.isfile(SCRIPT):
        print(f'  FAIL  {SCRIPT} not found')
        return 1
    with open(SCRIPT, encoding='utf-8', errors='replace') as fh:
        src = fh.read()
    assert_firewall_off(src, 'stage-oem.sh')

    print('== the staged image on the share agrees ==')
    if not os.path.isfile(STAGED):
        print(f'  SKIP  {STAGED} not present (share not mounted)')
    else:
        with open(STAGED, encoding='latin1') as fh:
            staged = fh.read()
        check('staged retroagent.reg is REGEDIT4', staged.startswith('REGEDIT4'))
        if re.search(r'"EnableFirewall"', staged):
            assert_firewall_off(staged, 'staged image')
        else:
            # Not a failure of the generator - it just has not been re-run.
            print('  WARN  staged retroagent.reg predates the firewall change;')
            print('        re-run scripts/pxe/stage-oem.sh so the image carries it')

    print()
    if fails:
        print(f'  {len(fails)} FAILED')
        return 1
    print('  all firewall checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
