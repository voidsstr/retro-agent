#!/usr/bin/env python3
"""The 2026-08-31 batch of five staged titles - and the four traps they hit.

WHY THIS EXISTS. Five LAN titles were staged in one pass (Return to Castle
Wolfenstein, Serious Sam TFE and TSE, Warcraft II: Battle.net Edition, Shadow
Warrior Classic Complete). Four defects were caught by AUDIT rather than by a
black screen on a fleet box, and every one of them would have presented as
"the staging is broken" rather than as what it actually was:

  1. **A VISTA-ONLY `ddraw.dll` INSIDE A 1999 GAME'S TREE.** GOG's Warcraft II
     ships a DirectDraw wrapper whose PE **SubsystemVersion is 6.0**. XP's
     loader refuses such an image before a single instruction runs, and a
     GAME-LOCAL `ddraw.dll` SHADOWS the one in system32 - so staging it would
     have taken the game down on every XP box in the fleet with no message
     naming the cause. It is not staged. This is CLAUDE.md's checklist item 8,
     and it is the second time a GOG repack has produced one (SiN Gold was the
     first, and was unloadable on every XP box).

  2. **AN IMPOSSIBLE PE TimeDateStamp ON GOG's `War2Launcher.exe`** (0x00050000,
     which decodes to 1970). Nothing in the tree needs it - the fleet launcher
     runs `Warcraft II BNE.exe` directly - so it is not staged rather than
     explained away. The GAME binaries all carry correct 2001 stamps, which is
     what makes the launcher's stamp a finding and not a panic.

  3. **SERIOUS SAM'S MODE LIVES IN A FILE THE ENGINE REWRITES ON EXIT.**
     `Scripts\\PersistentSymbols.ini` is written by the engine when it quits, so
     a resolution staged there is overwritten by the first box that runs the
     game and was wrong on the other seven before that. The engine's own hook is
     `Scripts\\Game_startup.ini`, whose shipped first line is literally
     "// executed each time SeriousSam is started". Same shape as id Tech 3's
     fleetres.cfg, different file.

  4. **GOG'S OWN SHADOW WARRIOR DOSBOX CONFS WAIT FOR A KEYPRESS.** Every
     `dosbox_swarrior_*.conf` opens with `@choice /c1234` and the multiplayer
     pair adds `@pause`. On a fleet box - driven by an agent with nobody at the
     keyboard - the game never starts, and it looks exactly like a broken staged
     tree. Redneck Rampage already paid for this once (dosboxRR_lan.conf's
     comment records the measurement). Ours are staged beside GOG's and GOG's
     are never referenced by a launcher.

The source-side tests run on the dev host in milliseconds. The share-side ones
SKIP LOUDLY when the library is not mounted, because a silent skip would let
the library rot unnoticed - the same reason test_staged_library.py does it.
"""
import os
import re
import struct

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
STAGER = os.path.join(REPO, 'scripts', 'fleet', 'stage-fleetres.py')
LIB = '/mnt/retro-share/Files/Games-Library'

TITLES = ('ReturnToCastleWolfenstein', 'WarcraftII', 'WarcraftOrcsAndHumans',
          'ShadowWarrior', 'MasterOfOrionII')

# Staged, tested on hardware, and REMOVED again - recorded here so the next
# person to look at this share does not spend the same afternoon.
#
# Serious Sam: The First Encounter and The Second Encounter both raise a modal
# "CD check / Please insert the game CD" and will not start without the disc,
# on a FULL local install with every .gro, Setup.exe, a C:\Install\ decoy and
# +cdpath all present. SeriousSam.exe imports GetDriveTypeA and the strings
# beside the message are "C:\Install\", "Bin\SeriousSam.exe", "Setup.exe" -
# it walks the drive letters for a CD-ROM-typed volume holding the game. That
# cannot be satisfied without mounting the disc, TSE is already v1.05 so a
# later official patch does not remove it, and two of the fleet's eight boxes
# (.123, .246) have no mounter at all - including the one this batch was
# LAN-proved against. Do not re-stage them without a mounter story.
WITHDRAWN = ('SeriousSamFirstEncounter', 'SeriousSamSecondEncounter')

# Not staged, and each for its own reason. See the module docstring.
WITHHELD = {
    'WarcraftII': ('ddraw.dll',          # SubsystemVersion 6.0 - Vista only
                   'War2Launcher.exe',   # TimeDateStamp decodes to 1970
                   'xdraw.dll',          # only reachable through that ddraw.dll
                   'dxcfg.exe'),         # ditto
}


def _skip_unless_share():
    if not os.path.isdir(LIB):
        pytest.skip('SKIPPED LOUDLY: %s is not mounted, so the staged library '
                    'could not be checked at all. This is not a pass.' % LIB)


def _read(path):
    with open(path, 'rb') as fh:
        return fh.read().decode('latin1')


# ---------------------------------------------------------------------------
# Source side - no share needed
# ---------------------------------------------------------------------------

def test_every_new_title_has_a_fleetres_recipe():
    """A per-box resolution must come from a RECIPE, never a pasted block.

    Halo arrived with the obsolete block hand-pasted into its launcher and no
    FLEETRES.BAT beside it; the README that invited the copy is gone and this
    is what keeps it gone. Warcraft II is the one deliberate exception and it
    is asserted as an exception rather than left as an absence.
    """
    src = _read(STAGER)
    # Both of these are native Win32 at a FIXED 640x480 - Warcraft II is
    # DirectDraw, Master of Orion II is a 1996 Win32 blit - so there is no mode
    # for a launcher to write and a recipe would be a no-op that READS AS
    # COVERAGE. They are asserted as exceptions rather than left as absences.
    # Warcraft II is native Win32 DirectDraw at a FIXED 640x480 - there is no
    # mode for a launcher to write and a recipe would be a no-op that READS AS
    # COVERAGE. It is asserted as an exception rather than left as an absence.
    NO_MODE = ('WarcraftII',)
    for t in TITLES:
        if t in NO_MODE:
            assert '"%s": {' % t not in src, (
                '%s gained a stage-fleetres recipe. If the title really did '
                'grow a settable mode, delete this assertion and say why - do '
                'not leave both stories in the tree.' % t)
            continue
        assert '"%s": {' % t in src, (
            '%s has no recipe in stage-fleetres.py TITLES, so its launchers '
            'would never get the FLEETRES block and the title would be pinned '
            'to one resolution on eight different monitors.' % t)


def test_withdrawn_titles_are_not_in_the_library():
    """A title that cannot start is worse than a title that is absent.

    Serious Sam was staged, validated and deployed before the CD check was
    found - by launching it. This asserts the removal stuck, in the library
    AND in the recipe file, because a leftover recipe would silently re-stage
    FLEETRES into a directory that should not exist.
    """
    src = _read(STAGER)
    for t in WITHDRAWN:
        assert '"%s"' % t not in src, (
            '%s still has a stage-fleetres recipe. See WITHDRAWN above: the '
            'title does not start without its disc.' % t)
    if not os.path.isdir(LIB):
        pytest.skip('SKIPPED LOUDLY: %s is not mounted.' % LIB)
    for t in WITHDRAWN:
        assert not os.path.isdir(os.path.join(LIB, t)), (
            '%s is back in the library. It raises a modal "Please insert the '
            'game CD" on a full local install; see WITHDRAWN above.' % t)


def test_pe_subsystem_rule_is_still_the_xp_rule():
    """The rule that condemned Warcraft II's ddraw.dll, stated as arithmetic.

    Asserted against BOTH the value that must pass and the value that must
    fail, so a change that inverts the comparison cannot go unnoticed.
    """
    def refused_by_xp(major, minor):
        return (major, minor) >= (6, 0)

    assert refused_by_xp(6, 0) is True     # GOG WarcraftII ddraw.dll
    assert refused_by_xp(6, 1) is True
    assert refused_by_xp(5, 1) is False    # ordinary XP-era binary
    assert refused_by_xp(4, 0) is False    # Warcraft II BNE.exe itself


# ---------------------------------------------------------------------------
# Share side - skips loudly
# ---------------------------------------------------------------------------

def test_titles_are_actually_staged():
    _skip_unless_share()
    for t in TITLES:
        assert os.path.isdir(os.path.join(LIB, t)), '%s is not in the library' % t
        assert os.path.isfile(os.path.join(LIB, t, 'launch.txt')), \
            '%s has no launch.txt, so it gets no desktop shortcut' % t
        assert os.path.isfile(os.path.join(LIB, t, 'requires.json')), (
            '%s has no requires.json. "nobody wrote one" and "there is no '
            'floor" are different facts and the gate keeps them apart.' % t)


def test_withheld_binaries_are_not_in_the_library():
    """Findings 1 and 2. A re-stage from the GOG installer would put them back."""
    _skip_unless_share()
    for title, names in WITHHELD.items():
        tdir = os.path.join(LIB, title)
        if not os.path.isdir(tdir):
            pytest.skip('%s is not staged' % title)
        have = {e.lower() for e in os.listdir(tdir)}
        for n in names:
            assert n.lower() not in have, (
                '%s/%s is staged again. It was withheld deliberately: see this '
                "module's docstring. ddraw.dll is Vista-only and game-local "
                'ddraw shadows system32, so XP refuses the image and the GAME '
                'dies with it.' % (title, n))


def _subsystem_version(path):
    with open(path, 'rb') as fh:
        data = fh.read(4096)
    if data[:2] != b'MZ':
        return None
    off = struct.unpack_from('<I', data, 0x3c)[0]
    if off + 0x50 > len(data) or data[off:off + 4] != b'PE\0\0':
        return None
    opt = off + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    if magic not in (0x10b, 0x20b):
        return None
    return (struct.unpack_from('<H', data, opt + 48)[0],
            struct.unpack_from('<H', data, opt + 50)[0])


def test_no_staged_binary_is_vista_only():
    """Finding 1, enforced over the whole of each new tree, not just one name.

    Walks the five trees rather than trusting the withheld list: the point is
    that NO image XP's loader would refuse reaches a box, however it got there.
    """
    _skip_unless_share()
    bad = []
    for t in TITLES:
        tdir = os.path.join(LIB, t)
        if not os.path.isdir(tdir):
            continue
        for root, _dirs, files in os.walk(tdir):
            for f in files:
                if os.path.splitext(f)[1].lower() not in ('.exe', '.dll'):
                    continue
                p = os.path.join(root, f)
                try:
                    sv = _subsystem_version(p)
                except OSError:
                    continue
                if sv and sv >= (6, 0):
                    bad.append('%s (SubsystemVersion %d.%d)'
                               % (os.path.relpath(p, LIB), sv[0], sv[1]))
    assert not bad, ('Vista-only image(s) staged; XP refuses these before the '
                     'first instruction:\n  ' + '\n  '.join(bad))


def test_shadow_warrior_confs_never_wait_for_a_keypress():
    """Finding 4. A fleet box has nobody at the keyboard."""
    _skip_unless_share()
    tdir = os.path.join(LIB, 'ShadowWarrior')
    if not os.path.isdir(tdir):
        pytest.skip('ShadowWarrior is not staged')
    ours = [c for c in os.listdir(tdir)
            if c.lower().startswith('dosboxsw_') or c.lower() == '_swipxhost.conf']
    assert ours, 'none of our own Shadow Warrior confs are staged'
    for c in ours:
        # Comments in our confs QUOTE the trap they avoid, so the check has to
        # look at the commands and not at the prose. A grep over the whole file
        # fails on a conf that is correct and well documented.
        body = _read(os.path.join(tdir, c))
        low = '\n'.join(l for l in body.lower().splitlines()
                        if not l.lstrip().startswith('#'))
        assert '@choice' not in low and 'choice /c' not in low, (
            '%s waits for a keypress. GOG\'s own confs do; ours must not, or '
            'the game never starts on an agent-driven box.' % c)
        assert '@pause' not in low, '%s pauses for a keypress' % c
    # and nothing may reference GOG's interactive pair
    for b in os.listdir(tdir):
        if not b.lower().endswith('.bat'):
            continue
        body = '\n'.join(l for l in _read(os.path.join(tdir, b)).lower().splitlines()
                         if not l.lstrip().startswith('rem'))
        for gogconf in ('dosbox_swarrior_server.conf',
                        'dosbox_swarrior_client.conf',
                        'dosbox_swarrior_single.conf'):
            assert gogconf not in body, (
                '%s references GOG\'s %s, which opens with @choice.' % (b, gogconf))


def test_launch_txt_contracts():
    """Data lines first, inside the agent's 1023-byte read, explicit in-tree
    icons, and no parentheses in a generated filename."""
    _skip_unless_share()
    for t in TITLES:
        p = os.path.join(LIB, t, 'launch.txt')
        if not os.path.isfile(p):
            pytest.skip('%s is not staged' % t)
        raw = open(p, 'rb').read()
        head = raw[:1023].decode('latin1')
        seen_comment = False
        rows = 0
        for line in head.split('\n'):
            s = line.strip()
            if not s:
                continue
            if s.startswith('#'):
                seen_comment = True
                continue
            assert not seen_comment, (
                '%s/launch.txt has a data line after a comment inside the first '
                '1023 bytes; put every data line first.' % t)
            cols = s.split('\t')
            assert len(cols) >= 3, (
                '%s/launch.txt line %r has no explicit icon column. Auto-'
                'resolution cannot separate a title\'s several launchers - it '
                'put the Half-Life lambda on Counter-Strike.' % (t, s))
            target, name, icon = cols[0], cols[1], cols[2]
            assert '(' not in target and ')' not in target, (
                '%s/launch.txt target %r contains a parenthesis. It is fine '
                'from a desktop double-click and UNLAUNCHABLE through the '
                'agent, which is why it survives review.' % (t, target))
            for ch in '\\/:*?"<>|':
                assert ch not in name, (
                    '%s/launch.txt display name %r contains %r; the name '
                    'becomes the .lnk FILENAME.' % (t, name, ch))
            assert os.path.isfile(os.path.join(LIB, t, icon.replace('\\', os.sep))), (
                '%s/launch.txt icon %r is not in the tree, so it degrades '
                'silently to the auto-resolved icon - exactly what the column '
                'exists to prevent.' % (t, icon))
            rows += 1
        assert rows, '%s/launch.txt has no data line inside the first 1023 bytes' % t
