#!/usr/bin/env python3
"""stage_dali_lan.py - LAN play for a Windows 9x box against the DOSBox boxes.

WHY. Every XP/Win7 box plays a DOS IPX game (Descent, Descent II) inside
DOSBox, whose IPX is a TUNNEL: IPX packets inside UDP to one machine running
`IPXNET STARTSERVER` (UDP 213). A Win98 box's own IPX (NWLINK) puts real IPX
frames on the wire, and the two never meet - nothing bridges them, and a
bridge would have to rewrite the node addresses Descent embeds in its own
packets. DALI (fragglet, GPL-3) is a REAL-DOS IPX driver that joins a DOSBox
IPX server directly, so the Win98 box simply becomes one more tunnel client.

DALI needs a packet driver, and a packet driver needs the network card, which
Windows owns - so the game runs in REAL DOS, through the same one-shot rundos
route the real-DOS titles use (stage_win9x_dos.py): the desktop launcher
copies the game to C:\\GAMES\\<dir> (real DOS on .243 does not see E:), arms
C:\\RUNDOS\\NEXT.BAT and restarts. In real DOS, RDLAN.BAT loads the packet
driver, gets an address by DHCP, asks for the host's IP (ASKIP, remembering
the last one in C:\\GAMES\\LANHOST.TXT - it WRITES HOSTIP.BAT itself: DOS's
line input echoes to stdout, so a redirect captured the keystrokes; with a
remembered host it joins by itself after a 10 s countdown - nobody can answer
a prompt on a box the agent cannot reach while real DOS runs), connects
DALI to <host>:213, checks
that IPX is really there (IPXCHK) and runs the game. Afterwards it unloads
DALI and WARM-REBOOTS (WBOOT): a Crynwr packet driver cannot unload itself,
and Windows is not started with one resident.

PROVEN 2026-10-01: the Win98 build VM in real DOS (NE2000 PCI, SLiRP) joined
.123's DOSBox Descent game this way and both machines flew in one match.

    python3 scripts/dosgames/stage_dali_lan.py --check          # does the share match?
    python3 scripts/dosgames/stage_dali_lan.py [--only Descent1] [--dry-run] [--via-smb]

The launch.txt row and the per-shortcut requires.json rule (max_os win9x) are
printed for the title's metadata owner; they are not written here.
"""
import argparse
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'scripts', 'fleet'))
from stage_win9x_dos import RD_HOOK  # noqa: E402  (ONE copy of the rundos line)
import libmeta  # noqa: E402  (co-owned launch.txt / requires.json merges)

# The desktop shortcut's rule: Win9x only - an NT box has the DOSBox and
# Rebirth LAN launchers, and no real-DOS route at all. The title floor applies.
RULE = {'max_os': 'win9x',
        'notes': 'LAN in REAL DOS through DALI against a DOSBox IPX host - the '
                 'Win9x box route (scripts/dosgames/stage_dali_lan.py).'}

LIB = '/mnt/retro-share/Files/Games-Library'
KIT = os.path.join(REPO, 'provisioning', 'dali')
# Every byte the kit ships lives in the repo (provisioning/dali, md5-pinned in
# its MD5SUMS.txt): the packet drivers used to be read from the share's
# Files/Utility/Retro Automation/dos-setup tree, which is a stale copy that the
# canonical Utility/Retro Automation no longer carries.
KIT_FILES = ('DALI.EXE', 'DHCP.EXE', 'ASKIP.COM', 'WBOOT.COM', 'IPXCHK.COM', 'COPYING.TXT',
             '3C509.COM', 'NE2000.COM')

# The packet drivers RDLAN.BAT tries, EACH ON ITS OWN VECTOR: a Crynwr driver
# that fails to find its card still disturbs the vector it was given, and a
# later driver on that vector comes up half-broken (PLAY.BAT's header, seen in
# DOSBox 2026-07-29). ORDER MATTERS MORE THAN PLAY.BAT SAYS: in the W98BUILD VM
# `NE2000 0x61 10 0x300` LOADED with no card at 0x300 (errorlevel 0) and DHCP
# then failed on every packet - measured 2026-10-01 (MTCP.CFG read PACKETINT
# 0x61). So every MEASURED card comes first and the ISA guesses come last:
# .243's 3Com EtherLink III ISA, then the VM's PCI RTL8029, whose BIOS puts it
# at E000 / IRQ 10 (PCINIC.COM, measured).
PKT_CANDIDATES = (
    ('3C509.COM', '0x60', ''),              # .243: 3Com EtherLink III ISA
    ('NE2000.COM', '0x61', '10 0xE000'),    # the W98BUILD VM (86Box ne2kpci)
    ('NE2000.COM', '0x62', '10 0x300'),     # ISA guesses, PLAY.BAT's set
    ('NE2000.COM', '0x63', '3 0x300'),
    ('NE2000.COM', '0x64', '5 0x280'),
)

# 'dir' is the 8.3 folder under C:\GAMES that real DOS runs the copy from. It
# must NOT be the title's own name: the W98BUILD VM's GamesDir is C:\Games, so
# C:\GAMES\DESCENT1 would be the source tree itself and xcopy refuses a
# self-copy (on .243 the source is E:\GAMES\Descent1, so only the VM sees it).
# Both Descents are bound to DOS/4GW 1.97, which dies with "DOS/4GW
# Professional error (2001): exception 0Dh (general protection fault)" when the
# box has 128 MB - .243 does; the build VM's 64 MB never showed it. Measured
# 2026-10-01: the P1 reset itself at Descent's start (twice), and the build VM
# at 128 MB printed exactly that GPF; the same VM ran Descent once DOS16M
# capped the extender at 32 MB (Descent needs 8). DOS16M=[mode][@range][:size]
# is DOS/4GW's own setting.
DOS16M_CAP = 'set DOS16M=:32M'

TITLES = {
    'Descent1': {
        'dir': 'D1LAN', 'name': 'Descent', 'bat': 'D1LAN.BAT',
        'label': 'Descent - LAN Game - real DOS', 'icon': 'DESCENT9.ICO',
        # The whole tree is 31 MB - copy it all (xcopy /D: only what changed).
        'copy': [('*.*', '/E', '')],
        # DESCENT.CFG is set up for DOSBox's SB at IRQ 7; the box's SB16 is at
        # IRQ 5 - the same switch "Play Descent - DOS.bat" makes.
        'pre': ['find "DigiIrq=7" DESCENT.CFG > nul',
                'if not errorlevel 1 copy DESCENT.SB5 DESCENT.CFG > nul',
                DOS16M_CAP],
        'game': 'DESCENTR.EXE',
        'mb': 32,
    },
    # Descent II's tree is 741 MB, 330 MB of it movies and 376 MB its disc
    # image - C: on .243 has ~450 MB. The real-DOS copy takes only what a
    # multiplayer game reads (~57 MB) and runs DESCENT2.EXE -nomovies (the
    # no-CD v1.2 exe carries the switch; without it the intro would be
    # looked for). The SB16 IRQ-5 switch is D2DOS.BAT's, and its HMI drivers.
    'Descent2': {
        'dir': 'D2LAN', 'name': 'Descent II', 'bat': 'D2LAN.BAT',
        'label': 'Descent II - LAN Game - real DOS', 'icon': 'DESCENTW.EXE',
        'copy': [('DESCENT2.EXE', '', ''), ('*.HOG', '', ''), ('*.HAM', '', ''),
                 ('*.PIG', '', ''), ('*.S11', '', ''), ('*.S22', '', ''),
                 ('*.MN2', '', ''), ('*.386', '', ''), ('*.PLR', '', ''),
                 ('DESCENT.CFG', '', ''), ('DESCENT.SB5', '', ''),
                 ('RDLAN.BAT', '', ''), ('RDHOOK.TXT', '', ''),
                 ('MISSIONS\\*.*', '', 'MISSIONS'), ('DALI\\*.*', '', 'DALI')],
        'pre': ['find "DigiIrq=7" DESCENT.CFG > nul',
                'if not errorlevel 1 copy DESCENT.SB5 DESCENT.CFG > nul',
                DOS16M_CAP],
        'game': 'DESCENT2.EXE -nomovies -noredbook',
        'mb': 60,
    },
}


def crlf(lines):
    for l in lines:
        assert len(l) <= 126, l
        assert '%~' not in l and not re.search(r'(?i)\bcd /d\b', l), l
        assert not re.search(r'[()]', l), l
        if l.lower().startswith('rem'):
            assert not re.search(r'[<>|]', l), l
    return ('\r\n'.join(lines) + '\r\n').encode('ascii')


def windows_bat(t):
    """The desktop launcher, COMMAND.COM dialect (Win98's shell)."""
    d, name = t['dir'], t['name']
    lines = ['@echo off',
             'rem %s - LAN game for a Windows 9x box, in REAL DOS through DALI.' % name,
             'rem The other boxes play in DOSBox, whose IPX is a tunnel over UDP 213 that',
             'rem Windows 98 IPX never meets. DALI is a real-DOS IPX driver that joins',
             'rem that tunnel - but it needs a packet driver, i.e. the network card, so',
             'rem this copies the game to C:\\GAMES\\%s - real DOS does not see E: -' % d,
             'rem arms the one-shot rundos line and restarts. RDLAN.BAT does the rest,',
             'rem then reboots into Windows. scripts/dosgames/stage_dali_lan.py made this.',
             'echo.',
             'echo  %s LAN game: the PC restarts into MS-DOS, joins a game hosted on' % name,
             'echo  another machine, and comes back to Windows when you quit.',
             'echo  Start the game on the HOSTING machine first - its screen shows its address.',
             'echo.',
             'choice /c:yn /t:y,10 Restart now',
             'if errorlevel 2 goto end',
             'echo Copying the game to C:\\GAMES\\%s ...' % d]
    for pattern, flags, sub in t['copy']:
        dst = 'C:\\GAMES\\%s\\%s' % (d, sub + '\\' if sub else '')
        lines += [('xcopy %s %s %s /I /D /Y /Q > nul' % (pattern, dst, flags)).replace('  ', ' '),
                  'rem XCOPY: 4 = could not start - no room, bad drive - and 5 = a write failed.',
                  'if errorlevel 4 goto nocopy']
    lines += ['if not exist C:\\GAMES\\%s\\RDLAN.BAT goto nocopy' % d,
              'if not exist C:\\GAMES\\%s\\DALI\\DALI.EXE goto nocopy' % d,
              'rem A fleet-wide lanhost.txt beside the games seeds the host the first time.',
              'if not exist C:\\GAMES\\LANHOST.TXT if exist ..\\LANHOST.TXT copy ..\\LANHOST.TXT C:\\GAMES > nul',
              'if not exist C:\\RUNDOS\\NUL md C:\\RUNDOS',
              'echo call C:\\GAMES\\%s\\RDLAN.BAT> C:\\RUNDOS\\NEXT.BAT' % d,
              'if not exist C:\\RUNDOS\\NEXT.BAT goto nocopy',
              'find "rundos v2" C:\\AUTOEXEC.BAT > nul',
              'if errorlevel 1 type RDHOOK.TXT >> C:\\AUTOEXEC.BAT',
              'rundll32.exe shell32.dll,SHExitWindowsEx 6',
              'goto end',
              ':nocopy',
              'echo.',
              'echo  COULD NOT COPY THE GAME TO C:\\GAMES\\%s - is C: full? It needs about %d MB.'
              % (d, t['mb']),
              'echo  Nothing was armed and Windows keeps running.',
              'pause',
              ':end',
              'rem CLS last: a Windows 9x DOS box closes on exit only if its screen is empty.',
              'cls']
    return crlf(lines)


def rdlan_bat(t):
    """What the rundos line runs, in REAL DOS. Every path that loaded a packet
    driver ends in WBOOT; a path that loaded nothing lets Windows start."""
    d, name = t['dir'], t['name']
    lines = ['@echo off',
             'rem %s LAN game in REAL DOS - run ONCE by the rundos line in AUTOEXEC.BAT.' % name,
             'rem Packet driver, DHCP, the host address, DALI to the host DOSBox IPX',
             'rem server on UDP 213, the game - then DALI /u and a warm reboot, because a',
             'rem packet driver cannot unload and Windows must not start with one.',
             'rem Generated by scripts/dosgames/stage_dali_lan.py.',
             'C:', 'cd \\GAMES\\%s\\DALI' % d,
             'set MTCPCFG=C:\\GAMES\\%s\\DALI\\MTCP.CFG' % d,
             'echo.', 'echo  Looking for the network card...']
    for i, (drv, vec, args) in enumerate(PKT_CANDIDATES):
        lines += [':try%d' % i,
                  ('%s %s %s' % (drv[:-4], vec, args)).strip() + ' > nul',
                  'if errorlevel 1 goto try%d' % (i + 1),
                  'echo PACKETINT %s> MTCP.CFG' % vec,
                  'goto pkt']
    lines += [':try%d' % len(PKT_CANDIDATES),
              'echo.',
              'echo  No network card found - the LAN game cannot start.',
              'pause',
              'goto done',
              ':pkt',
              'echo HOSTNAME RETRODOS>> MTCP.CFG',
              'DHCP',
              'if errorlevel 1 goto nodhcp',
              ':ask',
              'if exist C:\\RUNDOS\\HOSTIP.BAT del C:\\RUNDOS\\HOSTIP.BAT',
              'ASKIP C:\\GAMES\\LANHOST.TXT C:\\RUNDOS\\HOSTIP.BAT',
              'if errorlevel 1 goto boot',
              'call C:\\RUNDOS\\HOSTIP.BAT',
              'DALI %HOSTIP% 213',
              'rem IPXCHK asks DOS whether an IPX driver is resident - DALI only stays',
              'rem resident once the host server answered.',
              'IPXCHK > nul',
              'if not errorlevel 1 goto play',
              'echo.',
              'echo  Could not join the IPX game on %HOSTIP% - is the game started there?',
              'choice /c:rq Retry, or Quit back to Windows',
              'if errorlevel 2 goto boot',
              'goto ask',
              ':nodhcp',
              'echo.',
              'echo  No DHCP answer - is the network cable plugged in?',
              'pause',
              'goto boot',
              ':play',
              'cd \\GAMES\\%s' % d] + list(t['pre']) + [
              t['game'],
              'cd \\GAMES\\%s\\DALI' % d,
              'DALI /u > nul',
              ':boot',
              'echo.',
              'echo  Restarting into Windows...',
              'WBOOT',
              ':done',
              'C:', 'cd \\']
    return crlf(lines)


def kit_files():
    out = {}
    pinned = dict(reversed(l.split()) for l in open(os.path.join(KIT, 'MD5SUMS.txt')))
    for f in KIT_FILES:
        data = open(os.path.join(KIT, f), 'rb').read()
        if hashlib.md5(data).hexdigest() != pinned.get(f):
            raise SystemExit('provisioning/dali/%s is not its pinned build (MD5SUMS.txt)' % f)
        out['DALI\\' + f] = data
    out['DALI\\README.TXT'] = crlf([
        'DALI kit - joins this box to the DOSBox IPX game hosted on another machine.',
        'DALI.EXE  fragglet/dali v0.3, GPL-3 - COPYING.TXT - github.com/fragglet/dali',
        'DHCP.EXE  mTCP DHCP client from the same release, GPL-3',
        '3C509.COM NE2000.COM  Crynwr packet drivers, GPL',
        'ASKIP.COM WBOOT.COM IPXCHK.COM  retro-agent scripts/dosgames/dali',
        'Staged by scripts/dosgames/stage_dali_lan.py - edit there, not here.'])
    return out


def title_files(title):
    t = TITLES[title]
    files = kit_files()
    files[t['bat']] = windows_bat(t)
    files['RDLAN.BAT'] = rdlan_bat(t)
    files['RDHOOK.TXT'] = RD_HOOK.encode('ascii')
    return files


def launch_row(title):
    t = TITLES[title]
    return '%s\t%s\t%s' % (t['bat'], t['label'], t['icon'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', action='append')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--library', default=LIB)
    try:
        import libwrite
        libwrite.add_arguments(ap)
    except ImportError:
        libwrite = None
    a = ap.parse_args()
    titles = a.only or sorted(TITLES)
    writer = None
    if not (a.check or a.dry_run):
        writer = libwrite.writer_for(a.library, via_smb=getattr(a, 'via_smb', False))
    stale = 0
    for title in titles:
        root = os.path.join(a.library, title)
        if not os.path.isdir(root):
            raise SystemExit('%s is not a staged title - this never creates one' % title)
        for rel, data in sorted(title_files(title).items()):
            path = os.path.join(root, *rel.split('\\'))
            try:
                have = open(path, 'rb').read()
            except OSError:
                have = None
            if have == data:
                continue
            stale += 1
            print('%s %s\\%s (%d bytes)' % ('STALE' if a.check else 'write', title, rel, len(data)))
            if writer:
                writer.write_bytes(path, data)
        # launch.txt and requires.json are co-owned: merged, never templated
        # (libmeta), and launch.txt is written LAST so no box syncs a row
        # whose launcher is not on the share yet.
        t = TITLES[title]
        for rel, merge in (('requires.json', lambda cur: libmeta.merge_requires(
                                cur, set_shortcuts={t['bat']: RULE})),
                           ('launch.txt', lambda cur: libmeta.set_launch_rows(
                                cur, [tuple(launch_row(title).split('\t'))]))):
            path = os.path.join(root, rel)
            cur = (open(path, 'rb').read() if os.path.exists(path) else b'').decode('latin-1')
            new = merge(cur)
            if rel == 'launch.txt':
                probs = libmeta.launch_problems(new)
                if probs:
                    raise SystemExit('%s/launch.txt would lose a shortcut: %s' % (title, probs))
            data = new.encode('latin-1')
            if data == cur.encode('latin-1'):
                continue
            stale += 1
            print('%s %s\\%s (merged)' % ('STALE' if a.check else 'write', title, rel))
            if writer:
                writer.write_bytes(path, data)
    if a.check and stale:
        print('%d file(s) differ from the generator' % stale)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
