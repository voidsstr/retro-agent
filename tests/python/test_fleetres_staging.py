#!/usr/bin/env python3
"""Per-box resolution (FLEETRES) — the two invariants that were bought on hardware.

WHY THIS EXISTS. One staged tree deploys to EIGHT machines with eight different
monitors: four 1920x1080 16:9 LCDs, and four CRTs of which two are 4:3 tubes
being driven at 5:4 (squashed). A resolution written into a staged config is
therefore wrong somewhere BY CONSTRUCTION, and until 2026-08-30 the whole
library was pinned at 1024x768 — Tiberian Sun at 640x480.

Two findings cost real measurement time and are easy to undo by accident:

  1. DOSBOX `fullresolution=original` CHANGES THE WHOLE DESKTOP to the DOS mode.
     A/B'd on .145 with Descent 1 and confirmed with DISPLAYCFG: with `original`
     the desktop itself drops to 640x480 and a 4:3 signal is handed to a 16:9
     panel — and it is left behind after a crash, which is why .123 and .240
     were both found sitting at 640x480 mid-survey. With `desktop` the desktop
     mode is kept and DOSBox pillarboxes correctly via aspect=true. But
     `original` is still the RIGHT answer on a CRT, so this cannot be a staged
     constant in either direction; it has to be written per box at launch.

  2. id Tech 3's r_mode / r_customwidth / r_customheight / r_fullscreen are
     CVAR_LATCH: they are read once, at renderer init. The staged
     baseq3\\autoexec.cfg's `seta r_mode "6"` runs after Com_StartupVariable and
     before R_Init, so it BEAT the command line — measured on .123, where
     `+set r_customwidth 1920` left the game at 1024x768. Deleting those setas
     is what makes the command line work, and it is only safe to delete them
     once the launcher supplies the mode. Both halves have to ship together or
     the fleet gets a windowed/640x480 Quake III.

These tests read the staging tool, not the share, so they run on the dev host in
milliseconds and fail even when the library is not mounted. The share-side
equivalents live in scripts/validate-staged-library.py (suite [6]).
"""
import importlib.util
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
STAGER = os.path.join(REPO, 'scripts', 'fleet', 'stage-fleetres.py')
FLEETRES_C = os.path.join(REPO, 'provisioning', 'fleetres', 'fleetres.c')
VALIDATOR = os.path.join(REPO, 'scripts', 'validate-staged-library.py')


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


sf = _load('stage_fleetres', STAGER)


# ---------------------------------------------------------------------------
# 1. DOSBox fullresolution
# ---------------------------------------------------------------------------

DOSBOX_TITLES = ('Descent1', 'Carmageddon1', 'RedneckRampage')


def test_every_dosbox_title_rewrites_fullresolution_at_launch():
    """Every DOSBox launcher must hand [sdl] fullresolution to FLEETRES."""
    for title in DOSBOX_TITLES:
        spec = sf.TITLES[title]
        assert spec['launchers'], title
        for name, rec in spec['launchers'].items():
            block = '\n'.join(rec['pre'])
            assert 'sdl fullresolution %FR_DOSFULLRES%' in block, (
                '%s/%s does not rewrite fullresolution — on an LCD the staged '
                '`original` retargets the whole desktop to 640x480' % (title, name))


def test_fullresolution_is_never_a_staged_constant():
    """No recipe may write a literal fullresolution value.

    `original` and `desktop` are each correct on half this fleet. Hard-coding
    either is the bug, not the fix — which is why the assertion is on the
    ABSENCE of a constant and not on the presence of one.
    """
    bad = re.compile(r'sdl\s+fullresolution\s+(?!%FR_DOSFULLRES%)', re.I)
    for title, spec in sf.TITLES.items():
        for name, rec in spec.get('launchers', {}).items():
            block = '\n'.join(rec['pre'])
            assert not bad.search(block), (
                '%s/%s pins fullresolution to a constant' % (title, name))


def test_block_falls_back_to_original_not_desktop():
    """If FLEETRES.EXE cannot run, fall back to DOSBox's own default.

    `original` is what DOSBox does with no conf at all, so a fallback of
    `original` cannot make a box worse than it was. `desktop` on a CRT would.
    """
    m = re.search(r'if not defined FR_DOSFULLRES set FR_DOSFULLRES=(\S+)',
                  sf.FLEETRES_BAT)
    assert m, 'FLEETRES.BAT has no FR_DOSFULLRES fallback'
    assert m.group(1) == 'original'


def test_fleetres_source_still_maps_lcd_to_desktop():
    """The measurement lives in fleetres.c; keep it from being inverted.

    LCD -> desktop (keep the desktop mode, pillarbox with aspect=true)
    CRT -> original (a CRT has no pixel grid; the native DOS mode is fine)
    """
    src = open(FLEETRES_C, 'rb').read().decode('latin1')
    m = re.search(r'FR_DOSFULLRES=%s\\n"\s*,\s*(\w+)\s*\?\s*"(\w+)"\s*:\s*"(\w+)"',
                  src)
    assert m, 'the FR_DOSFULLRES emit line moved — re-read it before editing'
    cond, when_true, when_false = m.groups()
    assert cond == 'lcd'
    assert (when_true, when_false) == ('desktop', 'original'), (
        'LCD/CRT mapping is inverted: an LCD getting `original` drops the whole '
        'desktop to 640x480')


# ---------------------------------------------------------------------------
# 2. id Tech 3 latched cvars
# ---------------------------------------------------------------------------

IDTECH3_TITLES = ('Quake3-TeamArena', 'SoldierOfFortune2', 'JediAcademy')

LATCHED = ('r_mode', 'r_customwidth', 'r_customheight', 'r_fullscreen')


def _launch_text(spec):
    """Every string a title's recipes contribute to its .bat files."""
    out = []
    for rec in spec.get('launchers', {}).values():
        out.extend(rec['pre'])
        if rec['sub']:
            out.append(rec['sub'][1])
    return '\n'.join(out)


def test_stripping_the_autoexec_requires_the_launcher_to_supply_the_mode():
    """THE invariant. A title may only lose `seta r_mode` from its staged
    autoexec.cfg if its launchers pass the mode on the command line — otherwise
    the fleet ships a Quake III that comes up windowed at the engine default."""
    for title, spec in sf.TITLES.items():
        if not spec.get('cfg_strip'):
            continue
        text = _launch_text(spec)
        if not text and spec.get('new'):
            # a title whose launchers are created whole rather than patched
            text = sf.idtech3_args()
        for cvar in LATCHED:
            assert '+set %s' % cvar in text, (
                '%s strips its staged autoexec.cfg but no launcher passes '
                '+set %s — that ships a windowed game to every box'
                % (title, cvar))


def test_command_line_uses_custom_mode_not_a_mode_index():
    """r_mode must be -1: the fixed table has no 1920x1080 entry, so a mode
    index cannot express what half this fleet needs."""
    args = sf.idtech3_args()
    assert '+set r_mode -1' in args
    assert '+set r_customwidth %FR_W%' in args
    assert '+set r_customheight %FR_H%' in args
    assert '+set r_fullscreen 1' in args


def test_generated_cfg_is_the_second_route_and_matches_the_command_line():
    """The staged autoexec.cfg execs fleetres.cfg LAST, which is the only route
    that survives a box whose own config already latched a mode. It must set the
    same thing the command line does, or the two routes disagree by box."""
    cfg = '\n'.join(sf.idtech3_cfg('baseq3'))
    assert 'seta r_mode "-1"' in cfg
    assert 'seta r_customwidth "%FR_W%"' in cfg
    assert 'seta r_customheight "%FR_H%"' in cfg
    assert 'seta r_fullscreen "1"' in cfg
    assert cfg.count('fleetres.cfg') == len(sf.idtech3_cfg('baseq3'))


def test_appended_exec_is_the_last_thing_the_autoexec_does():
    """`exec fleetres.cfg` has to come after whatever the file set earlier —
    a cvar set twice in one pass keeps the LAST value."""
    out = sf.Runner._append_exec('seta r_colorbits "32"\n')
    assert out.rstrip().endswith('exec fleetres.cfg')
    assert sf.Runner._append_exec(out) == out, 'appending twice must be a no-op'


def test_idtech2_uses_the_fixed_mode_table():
    """id Tech 2 (Quake II, SiN, Soldier of Fortune) has gl_mode indices and NO
    custom mode and no 16:9 entry, so it gets the 4:3 answer, never FR_W."""
    cfg = '\n'.join(sf.q2_cfg('baseq2'))
    assert 'set gl_mode "%FR_Q2MODE%"' in cfg
    assert '%FR_W%' not in cfg and '%FR_H%' not in cfg


# ---------------------------------------------------------------------------
# 3. the block itself
# ---------------------------------------------------------------------------

def test_every_variable_a_recipe_uses_has_a_fallback():
    """A %FR_X% with no `if not defined` fallback expands to NOTHING on a box
    where FLEETRES.EXE is missing — `-w  -h ` on a command line, silently."""
    used = set()
    for spec in sf.TITLES.values():
        for rec in spec.get('launchers', {}).values():
            for s in rec['pre'] + ([rec['sub'][1]] if rec['sub'] else []):
                used.update(re.findall(r'%(FR_[A-Z0-9_]+)%', s))
    used.update(re.findall(r'%(FR_[A-Z0-9_]+)%', sf.NEW_GLQUAKE))
    used.update(re.findall(r'%(FR_[A-Z0-9_]+)%', sf.NEW_Q2))
    used.update(re.findall(r'%(FR_[A-Z0-9_]+)%', sf.NEW_IDTECH3))
    used.update(re.findall(r'%(FR_[A-Z0-9_]+)%', sf.idtech3_args()))
    have = set(re.findall(r'if not defined (FR_[A-Z0-9_]+) set',
                          sf.FLEETRES_BAT))
    assert used <= have, 'no fallback for %s' % sorted(used - have)


def test_no_generated_filename_contains_parentheses():
    """A .bat whose name contains ( or ) cannot be launched through the agent —
    and works fine from a desktop double-click, so it survives review."""
    for title, spec in sf.TITLES.items():
        for name in list(spec.get('launchers', {})) + list(spec.get('new', {})):
            assert '(' not in name and ')' not in name, '%s/%s' % (title, name)


def test_launch_txt_rows_carry_an_explicit_icon():
    """The third field exists because auto-resolution picks wrongly (Counter-
    Strike wore the Half-Life lambda; System Shock 2 wore a CD-Cops loader)."""
    for title, spec in sf.TITLES.items():
        rows = list(spec.get('launch_txt', []))
        if 'launch_txt_line0' in spec:
            rows.append(spec['launch_txt_line0'])
        for row in rows:
            assert len(row) == 3 and row[2], '%s: %r has no icon field' % (title, row)
            bad = [c for c in '\\/:*?"<>|' if c in row[1]]
            assert not bad, ('%s: display name %r contains %s, which the agent '
                             'turns into an illegal .lnk filename'
                             % (title, row[1], bad))


# ---------------------------------------------------------------------------
# 4. the share-side validator actually fires (a check that never fires is a lie)
# ---------------------------------------------------------------------------

vl = _load('validate_staged_library', VALIDATOR)


def _title(tmp_path, name, files):
    d = tmp_path / name
    d.mkdir(parents=True, exist_ok=True)
    for rel, body in files.items():
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(body.encode('latin1') if isinstance(body, str) else body)
    return d


def test_validator_rejects_a_staged_latched_r_mode(tmp_path):
    _title(tmp_path, 'T', {
        'launch.txt': 'Play T.bat\tT\tt.exe\r\n',
        'Play T.bat': '@echo off\r\nstart "" t.exe\r\n',
        't.exe': '',
        'baseq3/autoexec.cfg': 'seta r_mode "6"\r\nseta r_colorbits "32"\r\n',
    })
    probs = vl.check_title(str(tmp_path), 'T')
    assert any(p.check == 'idtech3-latch' and p.severity == 'fail' for p in probs)


def test_validator_rejects_a_dosbox_conf_nobody_rewrites(tmp_path):
    _title(tmp_path, 'D', {
        'launch.txt': 'Play D.bat\tD\td.ico\r\n',
        'Play D.bat': '@echo off\r\nDOSBox.exe -conf "..\\d.conf"\r\n',
        'd.ico': '',
        'd.conf': '[sdl]\r\nfullscreen=true\r\nfullresolution=original\r\n',
    })
    probs = vl.check_title(str(tmp_path), 'D')
    assert any(p.check == 'fleetres-dosbox' and p.severity == 'fail' for p in probs)


def test_validator_accepts_the_rewritten_form(tmp_path):
    _title(tmp_path, 'D2', {
        'launch.txt': 'Play D.bat\tD\td.ico\r\n',
        'Play D.bat': ('@echo off\r\ncall "%~dp0FLEETRES.BAT"\r\n'
                       'if exist "%~dp0FLEETRES.EXE" "%~dp0FLEETRES.EXE" -ini '
                       '"%~dp0d.conf" sdl fullresolution %FR_DOSFULLRES%\r\n'
                       'DOSBox.exe -conf "..\\d.conf"\r\n'),
        'd.ico': '',
        'd.conf': '[sdl]\r\nfullscreen=true\r\nfullresolution=original\r\n',
        'FLEETRES.EXE': '',
        'FLEETRES.BAT': sf.FLEETRES_BAT,
    })
    probs = vl.check_title(str(tmp_path), 'D2')
    assert not [p for p in probs if p.severity == 'fail'], [p.detail for p in probs]


def test_validator_rejects_a_half_staged_fleetres(tmp_path):
    """FLEETRES.EXE without FLEETRES.BAT, or a launcher expanding %FR_W% with no
    call — both leave the game starting with an EMPTY resolution argument."""
    _title(tmp_path, 'H', {
        'launch.txt': 'Play H.bat\tH\th.exe\r\n',
        'Play H.bat': '@echo off\r\nstart "" h.exe -w %FR_W% -h %FR_H%\r\n',
        'h.exe': '',
        'FLEETRES.EXE': '',
    })
    probs = vl.check_title(str(tmp_path), 'H')
    checks = {p.check for p in probs if p.severity == 'fail'}
    assert 'fleetres' in checks
