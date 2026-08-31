#!/usr/bin/env python3
"""The DOS-native lane: a staged title declares which file real DOS should run.

WHY THIS EXISTS
===============
Five staged titles carry the game's own DOS executable, and every one of them is
reached on every box through a `Play <Game>.bat` that starts the DOSBox staged
beside it. DOSBox needs roughly a gigahertz of host CPU to emulate a 486, so on
the fleet's genuine Pentium 1 (192.168.1.243, a 1997 Compaq Deskpro 2000 running
Windows 98 SE) the capability gate refuses all four DOSBox titles — while the
binaries those emulators are running are NATIVE to that machine.

Two separate defects made that unfixable, and this file pins both fixes.

1. THE EMULATOR'S COST WAS STATED AS THE GAME'S. Descent 1's requires.json said
   `min_cpu_mhz: 350` at the TITLE level, with the note "the floor is the
   emulator's host cost". That is true of the DOSBox shortcuts and of nothing
   else, so the whole 31 MB tree was refused on the one machine that can run the
   game at full speed — and Descent 1's own FAQ, staged in that tree, states the
   requirement as "486 or Pentium processor, 8 MB RAM". The floor now sits on
   the shortcuts that pay it.

2. THE DOS MENU COULD NOT FIND THE DOS BUILD. DOSGAME.EXE (the real-mode menu
   the agent stages on Win9x boxes) already scans C:\\GAMES, which is exactly
   where GAMESYNC deploys a staged tree — but it infers the launcher from 8.3
   names, and a staged tree is built for WINDOWS. Measured in DOSBox against the
   real file lists (scripts/dosgames/tests/test_pick_outcomes.sh):

       C:\\GAMES\\QUAKE1    -> GLQUAKE.EXE   a Win32 PE
       C:\\GAMES\\DESCENT1  -> DESCENT1.BAT  a cmd.exe batch ("cd /d")

   Neither is a bug in the heuristic. So the tree says it, in DOSGAME.TXT.

These tests run on the dev host with no share and no DOS toolchain; the
share-side assertions SKIP loudly rather than passing quietly, because a silent
skip is how a library rots.
"""
import importlib.util
import json
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
STAGER = os.path.join(REPO, 'scripts', 'fleet', 'stage-dosnative.py')
VALIDATOR = os.path.join(REPO, 'scripts', 'validate-staged-library.py')
DOSGAME_C = os.path.join(REPO, 'scripts', 'dosgames', 'dosgame.c')
PICK_TEST = os.path.join(REPO, 'scripts', 'dosgames', 'tests',
                         'test_pick_outcomes.sh')
LIB = '/mnt/retro-share/Files/Games-Library'


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


sd = _load('stage_dosnative', STAGER)
vl = _load('validate_staged_library', VALIDATOR)


# --- fixture executables ----------------------------------------------------
# Minimal but REAL headers, because the whole point of the check is that it
# reads the image rather than the file name.

def _mz(path):
    """A plain DOS MZ image (e_lfanew points nowhere useful)."""
    head = bytearray(0x40)
    head[0:2] = b'MZ'
    head[0x3C:0x40] = (0).to_bytes(4, 'little')
    path.write_bytes(bytes(head) + b'\0' * 64)


def _pe(path):
    """A Win32 PE - what must never be declared as a DOS launcher."""
    head = bytearray(0x80)
    head[0:2] = b'MZ'
    head[0x3C:0x40] = (0x80).to_bytes(4, 'little')
    path.write_bytes(bytes(head) + b'PE\0\0' + b'\0' * 200)


def _title(tmp_path, name, files, launch='PLAY.BAT\tPlay\n', decl=None):
    """Build a minimal staged title and return (library_root, title_dir)."""
    lib = tmp_path / 'lib'
    tdir = lib / name
    tdir.mkdir(parents=True)
    for fn, kind in files.items():
        if kind == 'pe':
            _pe(tdir / fn)
        elif kind == 'mz':
            _mz(tdir / fn)
        else:
            (tdir / fn).write_text('@echo off\n')
    (tdir / 'launch.txt').write_text(launch)
    if decl is not None:
        (tdir / 'DOSGAME.TXT').write_text(decl)
    return str(lib), name


def _fails(lib, title, check='dosgame.txt'):
    return [p for p in vl.check_title(lib, title)
            if p.severity == 'fail' and p.check == check]


# ---------------------------------------------------------------------------
# 1. The validator must be able to SAY NO. Every case below is a way the
#    declaration could be worse than the guess it replaces - which is the only
#    way this feature can hurt.
# ---------------------------------------------------------------------------

def test_declaring_a_windows_pe_is_a_failure(tmp_path):
    """The exact bug the feature exists to remove, arrived at from the other
    side: a declaration is only useful if it cannot re-create it."""
    lib, t = _title(tmp_path, 'QUAKE1',
                    {'PLAY.BAT': 'bat', 'GLQUAKE.EXE': 'pe', 'QUAKE.EXE': 'mz'},
                    decl='GLQUAKE.EXE\tQuake\n')
    bad = _fails(lib, t)
    assert bad, 'a PE declared as the DOS launcher must FAIL'
    assert 'PE' in bad[0].detail

    # ...and the correct declaration in the same tree must pass.
    lib, t = _title(tmp_path / 'ok', 'QUAKE1',
                    {'PLAY.BAT': 'bat', 'GLQUAKE.EXE': 'pe', 'QUAKE.EXE': 'mz'},
                    decl='QUAKE.EXE\tQuake\n')
    assert not _fails(lib, t)


def test_a_declaration_naming_a_missing_file_is_a_failure(tmp_path):
    lib, t = _title(tmp_path, 'DESCENT1',
                    {'PLAY.BAT': 'bat', 'DESCENTR.EXE': 'mz'},
                    decl='NOTHERE.EXE\tDescent\n')
    assert _fails(lib, t), 'a declaration naming nothing must FAIL'


def test_a_non_8_3_launcher_name_is_a_failure(tmp_path):
    """Real DOS sees an 8.3 alias, so a long name could never match. This is the
    same class as the file's OWN name being DOSGAME.TXT rather than
    dosnative.txt, which would arrive as DOSNAT~1.TXT."""
    lib, t = _title(tmp_path, 'DESCENT1',
                    {'PLAY.BAT': 'bat', 'DescentRegistered.exe': 'mz'},
                    decl='DescentRegistered.exe\tDescent\n')
    assert _fails(lib, t)


def test_a_parenthesis_in_the_launcher_is_a_failure(tmp_path):
    lib, t = _title(tmp_path, 'DESCENT1',
                    {'PLAY.BAT': 'bat', 'D1 (DOS).EXE': 'mz'},
                    decl='D1 (DOS).EXE\tDescent\n')
    assert _fails(lib, t)


def test_an_empty_declaration_is_a_failure(tmp_path):
    """Comments only. DOSGAME.EXE would fall back to guessing, silently."""
    lib, t = _title(tmp_path, 'DESCENT1',
                    {'PLAY.BAT': 'bat', 'DESCENTR.EXE': 'mz'},
                    decl='# nothing to see here\n\n')
    assert _fails(lib, t)


def test_a_title_with_no_declaration_is_not_penalised(tmp_path):
    """Absent data never blocks a title - the same rule the capability gate
    runs on. Most of the library is Windows-only and will never carry one."""
    lib, t = _title(tmp_path, 'MAXPAYNE', {'PLAY.BAT': 'bat'})
    assert not _fails(lib, t)


# ---------------------------------------------------------------------------
# 2. The stager verifies the POST-CONDITION, not that a write returned.
# ---------------------------------------------------------------------------

def test_stager_classifies_images_by_header_not_by_name(tmp_path):
    pe, mz = tmp_path / 'GLQUAKE.EXE', tmp_path / 'QUAKE.EXE'
    _pe(pe)
    _mz(mz)
    assert sd.dos_image(str(pe)) == 'PE'
    assert sd.dos_image(str(mz)) == 'MZ'
    assert sd.dos_image(str(tmp_path / 'nope.exe')) is None


def test_stager_lookup_is_case_insensitive(tmp_path):
    """We are a Linux host reading a Windows tree; a case-sensitive miss would
    report a staged file as absent, which this repo has paid for three times."""
    (tmp_path / 'Descent1.bat').write_text('x')
    assert sd.find_ci(str(tmp_path), 'DESCENT1.BAT') is not None


def test_every_withheld_title_says_why():
    """'we looked and it cannot work yet' and 'we never looked' are different
    facts, and only one of them is finished work."""
    assert sd.WITHHELD, 'the withheld list must not be quietly emptied'
    for title, (launcher, why) in sd.WITHHELD.items():
        assert launcher and len(why) > 40, title
        assert title not in sd.DECLARE, title


# ---------------------------------------------------------------------------
# 3. dosgame.c really reads it, and the DOS-side name is 8.3.
# ---------------------------------------------------------------------------

def test_dosgame_c_reads_the_declaration_before_guessing():
    src = open(DOSGAME_C, encoding='utf-8', errors='replace').read()
    assert '#define DECL_FILE "DOSGAME.TXT"' in src, \
        'the declaration file name must stay 8.3 - a longer one reaches DOS ' \
        'as a mangled alias that depends on what else is in the directory'
    assert 'read_declared(full, best, decl_title)' in src, \
        'scan_game_dir must consult the declaration'
    # The declaration must not be honoured when the file it names is absent:
    # a tree that was gated out or copied short has to degrade to the guess,
    # not to a launcher that cannot start.
    i = src.index('static int read_declared')
    body = src[i:i + 2000]
    assert 'file_exists(fulldir, exe)' in body


def test_the_dos_side_outcome_is_asserted_in_dosbox():
    """The evidence for this whole feature is an emulator run, not a comment."""
    t = open(PICK_TEST, encoding='utf-8', errors='replace').read()
    assert 'DOSGAME.TXT' in t
    assert 'not_expect QUAKE1' in t and 'expect     QUAKE1D  QUAKE.EXE' in t


def test_gamesync_deploys_where_the_dos_menu_looks():
    """The whole DOS lane rests on one coincidence that nothing else asserts:
    GAMESYNC's destination and DOSGAME.EXE's default scan root are the same
    directory. Either could be moved by someone who has never heard of the
    other, and the symptom would be a DOS menu that lists no games at all -
    with both components working exactly as designed.
    """
    gs = open(os.path.join(REPO, 'agent', 'src', 'gamesync.c'),
              encoding='utf-8', errors='replace').read()
    assert '#define GS_DEST            "C:\\\\Games"' in gs, \
        'GAMESYNC no longer deploys to C:\\Games - update DOSGAME.EXE\'s scan root'
    dg = open(DOSGAME_C, encoding='utf-8', errors='replace').read()
    assert 'cfg_scan[MAX_PATH_L * 2] = "C:\\\\GAMES;C:\\\\"' in dg, \
        'DOSGAME.EXE no longer scans C:\\GAMES by default - the staged games ' \
        'it is meant to find land there'


# ---------------------------------------------------------------------------
# 4. The share as it actually stands. SKIPS loudly.
# ---------------------------------------------------------------------------

_share = pytest.mark.skipif(not os.path.isdir(LIB),
                            reason='staged library not mounted at %s - the '
                                   'share side was NOT checked' % LIB)


def _requires(title):
    with open(os.path.join(LIB, title, 'requires.json'),
              encoding='utf-8', errors='replace') as fh:
        return json.load(fh)


@_share
def test_share_declarations_are_current_and_name_dos_images():
    for title, (launcher, _menu, _why) in sd.DECLARE.items():
        tree = sd.find_ci(LIB, title)
        assert tree, title
        decl = sd.find_ci(tree, 'DOSGAME.TXT')
        assert decl, '%s carries no DOSGAME.TXT - run stage-dosnative.py' % title
        first = [l for l in open(decl, encoding='latin1').read().splitlines()
                 if l.strip() and not l.strip().startswith('#')][0]
        assert first.split('\t')[0].strip().upper() == launcher.upper()
        exe = sd.find_ci(tree, launcher)
        assert sd.dos_image(exe) in ('MZ', 'LE', 'LX'), \
            '%s declares %s, which is not a DOS image' % (title, launcher)


@_share
def test_withheld_titles_carry_no_declaration():
    for title in sd.WITHHELD:
        tree = sd.find_ci(LIB, title)
        if tree is None:
            continue
        assert sd.find_ci(tree, 'DOSGAME.TXT') is None, \
            ('%s was declared without its CD-image problem being solved - it '
             'imgmounts a disc at launch and real DOS has no mounter staged'
             % title)


# The four titles whose ONLY Windows path is DOSBox. Each carries the game's own
# DOS binary, so the emulator's host cost must never be stated as the game's.
DOSBOX_TITLES = ('Descent1', 'Descent2', 'Carmageddon1', 'RedneckRampage')

# A 1995-97 DOS game's published minimum is a 486 or a Pentium 90. Anything at
# or above this at the TITLE level is an emulator cost wearing the game's name,
# and it refuses the tree on the one machine that runs the game natively.
EMULATOR_FLOOR_MHZ = 200


@_share
def test_the_dosbox_host_cost_is_per_shortcut_not_per_title():
    for title in DOSBOX_TITLES:
        doc = _requires(title)
        have = doc.get('min_cpu_mhz', 0)
        assert have < EMULATOR_FLOOR_MHZ, (
            "%s states min_cpu_mhz %s at the TITLE level. That is DOSBox's "
            "host cost, not the game's: the DOS binary in this tree is native "
            "to a Pentium, and a title-level floor refuses the whole tree on "
            "the box that can run it. Put it on the shortcuts that start "
            "DOSBox." % (title, have))
        shortcuts = doc.get('shortcuts') or {}
        assert shortcuts, '%s must state its emulator cost per shortcut' % title
        # Every shortcut in launch.txt must have a rule; a missed one inherits
        # the (now low) title floor and would be offered on a machine that
        # cannot start it.
        with open(os.path.join(LIB, title, 'launch.txt'),
                  encoding='latin1') as fh:
            targets = [l.split('\t')[0].strip()
                       for l in fh.read(1023).splitlines()
                       if l.strip() and not l.strip().startswith('#')]
        keys = {k.lower() for k in shortcuts}
        for tgt in targets:
            assert tgt.lower() in keys, \
                '%s: launch.txt line %r has no rule in requires.json' % (title, tgt)


@_share
def test_descent_rebirth_declares_the_cmov_floor_it_actually_has():
    """DXX-Rebirth's own .exe has zero CMOV; its LOAD-TIME imports do not.

    SDL.dll (286 CMOV) and SDL_mixer.dll (117) are in d1x-rebirth.exe's import
    table, so they map at process start. SDL_mixer.dll and libmikmod-2.dll (544)
    contain NO cpuid at all - no dispatch, so the i686 baseline is
    unconditional. SDL.dll's MMX *is* dispatched (14 cpuid sites,
    SDL_HasMMX/SDL_HasSSE exported), which is why mmx must NOT be declared:
    over-declaring refuses a title on machines that run it perfectly.
    """
    for title, shortcut in (('Descent1', 'Play Descent - Rebirth.bat'),
                            ('Descent2', 'Play Descent 2.bat')):
        sc = (_requires(title).get('shortcuts') or {})[shortcut]
        feats = [f.lower() for f in sc.get('cpu_features', [])]
        assert 'cmov' in feats, '%s/%s must declare cmov' % (title, shortcut)
        assert 'mmx' not in feats, \
            ('%s/%s must NOT declare mmx - SDL dispatches it at runtime, and a '
             'floor invented from an instruction count refuses boxes that run '
             'the title fine' % (title, shortcut))


@_share
def test_descent2_disc_mount_is_per_shortcut():
    """It was title-level, and shortcut rules INHERIT the title level - so on
    .123 and .246 (no virtual mounter) BOTH shortcuts were suppressed, including
    the DXX-Rebirth one whose own launcher header says 'NO DISC, NO MOUNTER'.
    Descent II had no desktop icon at all on those boxes and nothing said why.
    """
    doc = _requires('Descent2')
    assert not doc.get('requires_capabilities'), \
        'Descent2 must not require a disc mounter at the title level'
    sc = doc['shortcuts']
    # PRESENCE decides, not value: an empty list CLEARS an inherited rule and
    # an absent list does not, and they are indistinguishable by value.
    assert sc['Play Descent 2.bat'].get('requires_capabilities') == []
    assert sc['Play Descent 2 - original Win95.bat'][
        'requires_capabilities'] == ['disc_mount']


# Titles the Pentium 1 is offered on CPU/RAM/GPU grounds and cannot possibly
# hold. Measured 2026-08-30: with no disk_mb on these four, that box was offered
# 4,594 MB against a 617 MB disk, and GAMESYNC would have discovered it one
# SMB1 file at a time. Sizes: ThiefGold 797, SiNGold 964, Shogo 1101,
# TiberianSun 1113. Adding the floor changed no other box - the next smallest
# fleet disk has 9 GB free - and changed the P1's plan from 5 marginal to 1.
NEEDED_A_DISK_FLOOR = ('ThiefGold', 'SiNGold', 'Shogo', 'TiberianSun')


@_share
def test_every_dos_title_states_a_disk_floor():
    """617 MB free on the Pentium 1 is the binding constraint, not its CPU.
    Without disk_mb the gate approves a 878 MB tree onto it and GAMESYNC finds
    out an hour of SMB1 later."""
    for title in DOSBOX_TITLES + NEEDED_A_DISK_FLOOR:
        assert _requires(title).get('disk_mb'), (
            '%s states no disk_mb. disk_mb exists to refuse a copy BEFORE the '
            'bandwidth is spent, and it fails open where free space cannot be '
            'measured - so there is no cost to stating it and a real cost to '
            'leaving it out.' % title)


if __name__ == '__main__':
    sys.exit(pytest.main([__file__, '-v']))


# ---------------------------------------------------------------------------
# 5. The fleet's DOSGAME.EXE and this repo's build must stay the same program.
#
#    They were not, between 2026-08-26 and 2026-08-31: the share carried 1,842
#    bytes -- a self-extracting-archive fix -- that existed in no commit, on no
#    branch and in no file on this host, and nothing said so for five days. The
#    source was recovered from the binary (see scripts/dosgames/README.md); the
#    tests below now pin the RESOLVED state, and each one names the old broken
#    value so a regression reads as a regression rather than as a new fact.
# ---------------------------------------------------------------------------

CHECK_PUBLISHED = os.path.join(REPO, 'scripts', 'dosgames', 'check-published.py')


DOSGAME_EXE = os.path.join(REPO, 'scripts', 'dosgames', 'dosgame.exe')


def test_the_recovered_feature_is_in_the_committed_build():
    """OLD (2026-08-26..08-30): these four strings existed ONLY inside a binary
    on the share - in no commit and in no file on this host. NOW the tracked
    build carries all four, which is what makes the fleet's copy reproducible.

    Checked against the BINARY, not the source: C string literals wrap across
    lines, so a source grep would be asserting the formatting rather than the
    program. The binary is the artifact the fleet actually runs."""
    cp = _load('check_published', CHECK_PUBLISHED)
    assert len(cp.FEATURE_MARKERS) == 4, \
        ('the four log strings of the recovered feature are the machine-'
         'checkable record that it exists - do not thin them out')
    built = open(DOSGAME_EXE, 'rb').read()
    for m in cp.FEATURE_MARKERS:
        assert isinstance(m, bytes)
        assert m in built, (
            '%r is NOT in the tracked dosgame.exe. Either the recovered '
            'self-extracting-archive fix has been deleted, or the tracked '
            'binary is stale - and its only symptom on a DOS box is a game '
            'menu that launches an installer instead of the game, for ever. '
            'Rebuild with `make -C scripts/dosgames`.' % m)


def test_the_divergence_check_now_fails_the_suite():
    """OLD: check-published.py was wired in WITHOUT --strict, because the
    divergence was a known unresolved fact and failing every run_all.sh over it
    would have trained everyone to ignore it. It is resolved, so the check is
    now a gate - which is the only thing that stops it happening twice."""
    body = open(CHECK_PUBLISHED, encoding='utf-8').read()
    assert 'return 1 if args.strict else 0' in body
    runner = open(os.path.join(REPO, 'scripts', 'dosgames', 'tests',
                               'run_dos_tests.sh'), encoding='utf-8').read()
    assert 'check-published.py' in runner, 'the suite must print the verdict'
    assert '--strict' in runner.split('check-published.py')[1][:40], \
        ('run_dos_tests.sh must invoke check-published.py --strict. Without '
         'it the repo and the share can drift again in either direction and '
         'nothing will say so.')


def test_the_content_test_reads_the_file_not_the_name():
    """The whole point of the recovered feature: a self-extracting download is
    an .EXE by name and a ZIP/LZH archive by content, so a name-based rule
    cannot see it. OLD: no signature test existed at all."""
    src = open(DOSGAME_C, encoding='utf-8', errors='replace').read()
    assert 'static int is_selfextract' in src
    assert "buf[i] == 0x50 && buf[i + 1] == 0x4b" in src, \
        'the ZIP local-header signature PK\\3\\4 is what identifies an SFX'
    assert "buf[i + 2] == 'h'" in src, 'the LZH -lh?- method id too'
    assert 'memmove(buf, buf + end - 4, 4)' in src, \
        ('chunks must overlap by 4 bytes or a signature straddling a 512-byte '
         'read boundary is invisible')
    for bound in ('SFX_MIN_BYTES', 'SFX_SCAN_BYTES'):
        assert bound in src, \
            ('%s bounds the content scan - this runs on a 486, and an '
             'unbounded read of every candidate in every directory is not '
             'affordable there' % bound)


# ---------------------------------------------------------------------------
# 6. The --check CONTRACT. A checker that can only say OK is the exact failure
#    this repo keeps paying for, so every one of these asserts a NON-zero exit.
#    (These paths were written before they were ever run; running them is what
#    this block is for.)
# ---------------------------------------------------------------------------

def _mzfile(path):
    head = bytearray(0x40)
    head[0:2] = b'MZ'
    head[0x3C:0x40] = (0).to_bytes(4, 'little')
    path.write_bytes(bytes(head) + b'\0' * 64)


def _stager(*args):
    import subprocess
    return subprocess.run([sys.executable, STAGER] + list(args),
                          capture_output=True, text=True)


def _fixture_lib(tmp_path, with_decl=None):
    """A library holding just the three declared titles' launchers."""
    lib = tmp_path / 'lib'
    for name, exe in (('Descent1', 'DESCENTR.EXE'), ('Descent2', 'DESCENT2.EXE'),
                      ('Quake1', 'QUAKE.EXE')):
        d = lib / name
        d.mkdir(parents=True)
        _mzfile(d / exe)
        if with_decl and name in with_decl:
            (d / 'DOSGAME.TXT').write_bytes(with_decl[name])
    return lib


def test_check_exits_nonzero_when_the_share_is_not_current(tmp_path):
    lib = _fixture_lib(tmp_path)
    r = _stager('--check', '--library', str(lib))
    assert r.returncode == 1, 'an unstaged library must FAIL --check'
    assert 'STALE' in r.stdout


def test_check_exits_nonzero_on_a_declaration_that_drifted(tmp_path):
    """Hand-edited, or generated by an older version of this tool. Byte-exact
    is the only comparison that catches a note nobody updated."""
    lib = _fixture_lib(tmp_path, {'Descent1': b'DESCENTR.EXE\tDescent\n'})
    r = _stager('--check', '--library', str(lib))
    assert r.returncode == 1
    assert 'different' in r.stdout


def test_check_exits_nonzero_when_a_declared_title_is_gone(tmp_path):
    r = _stager('--check', '--library', str(tmp_path))
    assert r.returncode == 1, 'a library missing every declared title must FAIL'
    assert 'FAIL' in r.stdout


def test_check_SKIPS_rather_than_failing_when_the_share_is_absent():
    """The dev host without the SMB mount must not fail; but it must SAY so,
    because a silent skip is how a library rots."""
    r = _stager('--check', '--library', '/nonexistent-retro-share')
    assert r.returncode == 0
    assert 'SKIP' in (r.stdout + r.stderr)


def test_check_passes_on_the_real_share_because_it_IS_current():
    """Paired with the failures above: proves the checker can also say yes."""
    if not os.path.isdir(LIB):
        pytest.skip('staged library not mounted at %s' % LIB)
    r = _stager('--check')
    assert r.returncode == 0, r.stdout + r.stderr
    assert '0 change(s)' in r.stdout
