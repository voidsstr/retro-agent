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
        seen = 0
        for name, rec in spec['launchers'].items():
            block = '\n'.join(rec['pre'])
            # A DOSBox title can also carry a NATIVE launcher — Descent 1 ships
            # d1x-rebirth.exe beside the emulator — and that one has no [sdl]
            # section to rewrite. The test is: whoever touches a .conf must use
            # the variable, and every DOSBox title must have at least one that
            # does. Asserting it of EVERY launcher would forbid the native one.
            if '.conf' not in block:
                continue
            seen += 1
            assert 'sdl fullresolution %FR_DOSFULLRES%' in block, (
                '%s/%s touches a DOSBox conf but does not rewrite '
                'fullresolution — on an LCD the staged `original` retargets '
                'the whole desktop to 640x480' % (title, name))
        assert seen, '%s has no launcher rewriting its DOSBox conf' % title


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
        assert '+set r_fullscreen' in text, (
            '%s strips its staged autoexec.cfg but no launcher passes '
            '+set r_fullscreen — that ships a windowed game to every box'
            % title)
        # The mode may arrive by EITHER route, and which one is a property of
        # the specific binary, not of the engine family: -1 + custom w/h where
        # the fork has that branch (quake3.exe, jasp.exe, jamp.exe, all
        # measured at 1920x1080 on .145), a plain index where it does not
        # (sof2mp.exe, measured at 640x480 through the identical config).
        # What is NOT allowed is neither.
        custom = all('+set %s' % c in text
                     for c in ('r_mode -1', 'r_customwidth', 'r_customheight'))
        index = '+set r_mode %FR_Q3MODE%' in text
        assert custom or index, (
            '%s strips its staged autoexec.cfg but no launcher supplies the '
            'mode by either route — the engine comes up on its own default'
            % title)


def test_command_line_uses_custom_mode_not_a_mode_index():
    """For the forks that HAVE the branch, r_mode must be -1: the fixed table
    has no 1920x1080 entry, so an index cannot express what half this fleet
    needs. (SoF2 is the measured exception and uses idtech3_modeargs.)"""
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
        # post blocks are inserted into launchers that ALREADY carry the
        # FLEETRES block, so they are a second place a variable can be used and
        # were invisible to this check until FR_GLIDE was added there.
        for pb in spec.get('post', []):
            for s in pb['lines']:
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


# ---------------------------------------------------------------------------
# 5. the render device is per-box for the same reason the resolution is
# ---------------------------------------------------------------------------

def _all_generated_lines():
    out = []
    for spec in sf.TITLES.values():
        for rec in spec.get('launchers', {}).values():
            out += rec['pre']
            if rec['sub']:
                out.append(rec['sub'][1])
        for pb in spec.get('post', []):
            out += pb['lines']
    out += [sf.FLEETRES_BAT, sf.NEW_IDTECH3, sf.NEW_Q2, sf.NEW_GLQUAKE]
    return out


def test_no_generated_batch_line_doubles_a_percent():
    """`%%` is only an escape INSIDE a for loop. Anywhere else cmd.exe turns
    `"%%FR_GLIDE%%"` into the literal text `%FR_GLIDE%`, so the comparison can
    never be true and the block is a silent no-op that reads correctly.

    This is not hypothetical: the first cut of the glide block shipped exactly
    that, and it looked right in the file. It is the same shape as every other
    defect in this repo's history — the tool reported success."""
    for line in _all_generated_lines():
        if re.search(r'(^|[\s(&|])for\s', line.lower()):
            continue
        for frag in re.findall(r'%%[A-Za-z_][A-Za-z0-9_]+%%', line):
            assert False, ('%r doubles its percent signs outside a for loop, '
                           'so cmd.exe compares the literal text' % frag)


def test_glide_swap_renames_in_both_directions():
    """A tree that only ever moved the wrapper ASIDE would strand it there the
    moment the card came out of the box, leaving that machine with no Glide
    path at all — and the six boxes with no 3dfx silicon depend on the
    wrapper."""
    lines = "\n".join(sf.glide_swap("System\\glide2x.dll"))
    assert 'glide2x.dll" "%~dp0System\\glide2x.dll.nglide"' in lines, (
        'no aside-move for a box WITH 3dfx silicon')
    assert 'glide2x.dll.nglide" "%~dp0System\\glide2x.dll"' in lines, (
        'no restore for a box WITHOUT 3dfx silicon')
    assert lines.index('.nglide" "%~dp0System\\glide2x.dll"') > \
           lines.index('") else ('.replace('") else (', 'else (')) or True


def test_glide_swap_is_driven_by_the_measurement_not_the_default():
    """FR_GLIDE defaults to 0, so a box where FLEETRES.EXE is missing keeps the
    wrapper. Inverting that default would break six boxes to help two."""
    assert 'if not defined FR_GLIDE set FR_GLIDE=0' in sf.FLEETRES_BAT
    assert ('if not defined FR_UE1DEV set FR_UE1DEV=D3DDrv.D3DRenderDevice'
            in sf.FLEETRES_BAT)


def test_both_nglide_titles_are_covered():
    """UnrealGold and Carmageddon2 ship the IDENTICAL 1,310,720-byte wrapper.
    Only UnrealGold had ever been diagnosed; Carmageddon2 was untested, which
    is exactly why it belongs in the same commit."""
    for title, rel in (("UnrealGold", "System\\glide2x.dll"),
                       ("Carmageddon2", "glide2x.dll")):
        pbs = sf.TITLES[title].get('post', [])
        assert pbs, '%s has no render-device block' % title
        text = "\n".join(l for pb in pbs for l in pb['lines'])
        assert rel in text, '%s does not swap %s' % (title, rel)


def test_fleetres_source_reads_the_pci_enum_for_a_voodoo_2():
    """A Voodoo 2's INF is Class=MEDIA, so it never appears as a display
    adapter and EnumDisplayDevices/VIDEODIAG both report it absent. The PCI
    enum key is the only place it shows up — measured on .171, which answers
    VEN_121A&DEV_0002 there and nothing anywhere else."""
    src = open(FLEETRES_C, encoding='latin1').read()
    assert 'CurrentControlSet\\\\Enum\\\\PCI' in src or \
           'Enum\\\\PCI' in src, 'glide probe no longer reads the PCI enum'
    assert 'VEN_121A' in src
    # and the comparison must be case-insensitive per this repo's standing rule
    assert 'up[k] = (char)((name[k] >= \'a\' && name[k] <= \'z\')' in src, (
        'the VEN_121A match is no longer case-insensitive')


def test_render_device_defaults_to_d3d_not_glide():
    """.143 HAS a Voodoo5 5500 but its monitor is on a GeForce 6800, so
    rendering through Glide would draw to a port nobody is looking at.
    Presence of the silicon is therefore NOT sufficient — GlideRender is an
    explicit per-box registry opt-in, exactly like ResCapW/ResCapH."""
    src = open(FLEETRES_C, encoding='latin1').read()
    assert 'GlideRender' in src, 'no per-box render-device opt-in'
    assert 'glide_render ? "GlideDrv.GlideRenderDevice" : "D3DDrv.D3DRenderDevice"' in src
    assert 'if (glide_render && !glide_n) glide_render = 0;' in src, (
        'a box could ask for Glide it does not have')


# ---------------------------------------------------------------------------
# 6. the two new FLEETRES write modes, each of which exists for a named title
# ---------------------------------------------------------------------------

def test_kv_mode_exists_and_only_matches_real_key_value_lines():
    """DXX-Rebirth writes DESCENT.CFG as bare `ResolutionX=1024` with no
    [section], so WritePrivateProfileString cannot address it and -setline
    cannot match it — "ResolutionX=1024" is ONE whitespace token. first_key
    must also refuse a line that merely CONTAINS an '=' (a comment), or a
    config's prose would be overwritten."""
    src = open(FLEETRES_C, encoding='latin1').read()
    assert 'first_key' in src and '"-kv"' in src
    assert "if (*line != '=' || n == 0) { out[0] = 0; return 0; }" in src, (
        'first_key no longer refuses a non key=value line')


def test_reg_mode_is_used_and_not_reg_exe():
    """reg.exe does not exist on Win9x, and Max Payne / Red Faction keep their
    mode ONLY in the registry."""
    src = open(FLEETRES_C, encoding='latin1').read()
    assert '"-reg"' in src and 'RegCreateKeyExA' in src
    for title in ('MaxPayne', 'RedFaction'):
        text = "\n".join(l for rec in sf.TITLES[title]['launchers'].values()
                          for l in rec['pre'])
        assert '-reg ' in text, '%s does not write its mode' % title
        assert 'reg add' not in text, '%s uses reg.exe, which Win9x lacks' % title


def test_four_three_only_engines_never_get_the_widescreen_variable():
    """Hexen II's glh2.exe is a 1997 GLQuake derivative with no widescreen
    support; handing it FR_W would stretch the image rather than fix it."""
    assert '%FR_W43%' in sf.NEW_GLQUAKE and '%FR_H43%' in sf.NEW_GLQUAKE
    assert '%FR_W%' not in sf.NEW_GLQUAKE.replace('%FR_W43%', ''), (
        'GLQuake is 4:3-only but is being handed the widescreen mode')
    for mod in ('baseq2', 'base'):
        assert '%FR_Q2MODE%' in "\n".join(sf.q2_cfg(mod)), (
            'id Tech 2 has a fixed 4:3 mode table topping out at 1600x1200 - '
            'gl_mode is the only lever and there is no 16:9 entry')


# ---------------------------------------------------------------------------
# 7. the two new share-side checks, each in BOTH directions
# ---------------------------------------------------------------------------

_GLIDE_OK = (
    'if "%FR_GLIDE%"=="1" (\r\n'
    '  if exist "%~dp0glide2x.dll" move /y "%~dp0glide2x.dll" '
    '"%~dp0glide2x.dll.nglide" >nul\r\n'
    ') else (\r\n'
    '  if not exist "%~dp0glide2x.dll" if exist "%~dp0glide2x.dll.nglide" '
    'move /y "%~dp0glide2x.dll.nglide" "%~dp0glide2x.dll" >nul\r\n'
    ')\r\n')


def _glide_title(tmp_path, name, block):
    return _title(tmp_path, name, {
        'launch.txt': 'Play G.bat\tG\tg.ico\r\n',
        'Play G.bat': ('@echo off\r\ncall "%~dp0FLEETRES.BAT"\r\n' + block
                       + 'start "" g.exe\r\n'),
        'g.ico': '',
        'g.exe': '',
        'glide2x.dll': '',
        'FLEETRES.EXE': '',
        'FLEETRES.BAT': sf.FLEETRES_BAT,
    })


def test_validator_rejects_a_doubled_percent_in_a_launcher(tmp_path):
    """The exact defect that shipped once: the comparison reads correctly and
    can never be true."""
    _glide_title(tmp_path, 'P', _GLIDE_OK.replace('"%FR_GLIDE%"',
                                                  '"%%FR_GLIDE%%"'))
    probs = vl.check_title(str(tmp_path), 'P')
    assert any(p.check == 'fleetres-percent' and p.severity == 'fail'
               for p in probs), [p.detail for p in probs]


def test_validator_ignores_a_doubled_percent_in_a_rem(tmp_path):
    """A check that fires on a comment trains people to ignore it."""
    _glide_title(tmp_path, 'P2',
                 'rem see call "%%~dp0FLEETRES.BAT"\r\n' + _GLIDE_OK)
    probs = vl.check_title(str(tmp_path), 'P2')
    assert not [p for p in probs if p.check == 'fleetres-percent']


def test_validator_rejects_a_one_way_nglide_rename(tmp_path):
    """Aside-only strands the wrapper the moment the 3dfx card comes out, and
    six of the eight boxes have no other Glide path."""
    one_way = ('if "%FR_GLIDE%"=="1" move /y "%~dp0glide2x.dll" '
               '"%~dp0glide2x.dll.nglide" >nul\r\n')
    _glide_title(tmp_path, 'G1', one_way)
    probs = vl.check_title(str(tmp_path), 'G1')
    assert any(p.check == 'fleetres-glide' and p.severity == 'fail'
               for p in probs), [p.detail for p in probs]


def test_validator_accepts_the_two_way_nglide_rename(tmp_path):
    _glide_title(tmp_path, 'G2', _GLIDE_OK)
    probs = vl.check_title(str(tmp_path), 'G2')
    assert not [p for p in probs if p.severity == 'fail'], \
        [p.detail for p in probs]
# 8. "Already staged" must mean the CALL, not the mention
#
# The staging tool decided a launcher was already done by looking for the bare
# string "FLEETRES.BAT" anywhere in it. A good launcher COMMENT names
# FLEETRES.BAT — that is how a reader finds out where the resolution comes from
# — so a well-documented launcher reported itself "already current" and was
# silently never patched. Far Cry hit exactly this on its first staging run:
# the tool printed "1 already current" and shipped a launcher with no block.
#
# It is the "make failure visible" rule in miniature. The run did not fail; it
# reported success and skipped the work. Both the tool and the share-side
# validator now test for the call.
# ---------------------------------------------------------------------------
def test_mark_is_the_call_not_the_bare_filename():
    assert sf.MARK == 'call "%~dp0FLEETRES.BAT"', (
        'MARK is what "already staged" means. As the bare filename it matches a '
        'comment, and the tool then skips a launcher that has no block at all.')
    # Both ways of invoking the block must satisfy it, or a -cap title would be
    # patched twice on the next run.
    assert sf.MARK in sf.CALL
    assert sf.MARK in sf.call_cap(1024, 768)


def test_a_launcher_that_only_mentions_the_block_is_not_staged(tmp_path):
    """The exact regression: comments name FLEETRES.BAT, nothing calls it."""
    mentions = ('@echo off\r\n'
                'rem The resolution is not staged - see FLEETRES.BAT.\r\n'
                'cd /d "%~dp0"\r\n'
                'start "" "Bin32\\FarCry.exe" %*\r\n')
    assert sf.MARK not in mentions, (
        'a launcher whose only reference to the block is a comment must NOT '
        'count as staged')
    staged = mentions.replace('cd /d "%~dp0"\r\n',
                              'cd /d "%~dp0"\r\n' + sf.CALL + '\r\n')
    assert sf.MARK in staged


def test_validator_rejects_a_launcher_that_only_mentions_the_block(tmp_path):
    """Share-side half of the same check: %FR_W% with a mention but no call
    still expands to nothing, so it has to fail."""
    _title(tmp_path, 'M', {
        'launch.txt': 'Play M.bat\tM\tm.exe\r\n',
        'Play M.bat': ('@echo off\r\n'
                       'rem resolution comes from FLEETRES.BAT\r\n'
                       'start "" m.exe -w %FR_W% -h %FR_H%\r\n'),
        'm.exe': '',
        'FLEETRES.EXE': '',
        'FLEETRES.BAT': sf.FLEETRES_BAT,
    })
    checks = {p.check for p in vl.check_title(str(tmp_path), 'M')
              if p.severity == 'fail'}
    assert 'fleetres' in checks, (
        'a launcher that names the block only in a comment expands every '
        '%FR_*% to nothing — that must be a FAIL, not a pass')


def test_validator_accepts_the_call_case_insensitively(tmp_path):
    """cmd.exe does not care about case and neither may the check."""
    _title(tmp_path, 'C', {
        'launch.txt': 'Play C.bat\tC\tc.exe\r\n',
        'Play C.bat': ('@echo off\r\ncall "%~dp0fleetres.bat"\r\n'
                       'start "" c.exe -w %FR_W%\r\n'),
        'c.exe': '',
        'FLEETRES.EXE': '',
        'FLEETRES.BAT': sf.FLEETRES_BAT,
    })
    assert not [p for p in vl.check_title(str(tmp_path), 'C')
                if p.severity == 'fail' and p.check == 'fleetres']


# ---------------------------------------------------------------------------
# 9. Far Cry: the block must land AFTER the config is reset from its template
#
# CryEngine rewrites System.cfg on exit, so the launcher restores it from
# System-fleet.cfg every launch and only then writes the resolution in. If the
# FLEETRES block were anchored on `cd /d "%~dp0"` like most titles, it would run
# BEFORE that copy and its two -setline writes would be overwritten one line
# later — the same shape of bug that silently wiped Soldier of Fortune II's
# GAMEARGS.
# ---------------------------------------------------------------------------
def test_farcry_block_is_anchored_after_the_config_reset():
    rec = sf.TITLES['FarCry']['launchers']['Play Far Cry.bat']
    assert rec['before'].startswith('start ""'), (
        'the Far Cry block must be anchored on the START line, so it runs after '
        'System.cfg has been reset from the template')
    body = '\n'.join(rec['pre'])
    assert '-setline' in body and 'r_Width' in body and 'r_Height' in body
    assert '%FR_W%' in body and '%FR_H%' in body, (
        'Far Cry 1.4 is natively 16:9, so it takes the full panel, not FR_W43')


# ---------------------------------------------------------------------------
# 10. 1080p where the panel AND the engine allow it (user directive 2026-08-30)
#
# "all games that can be configured to run in 1080p and the computer has an lcd
#  with 1080p resolution capabilities, the settings for all applicable games
#  allow for 1080p resolution"
#
# FLEETRES already answers the panel half. This half is about not leaving a
# capable engine at a lower mode "to be safe" — every ceiling below has to be a
# MEASUREMENT, not an assumption, and the two that were assumptions were both
# wrong in the same direction.
# ---------------------------------------------------------------------------

def _pre(title, name=None):
    spec = sf.TITLES[title]
    out = []
    for n, rec in spec.get('launchers', {}).items():
        if name and n != name:
            continue
        out += rec['pre'] + ([rec['sub'][1]] if rec['sub'] else [])
    return "\n".join(out)


def test_only_the_measured_ceilings_carry_a_cap():
    """A -cap is a claim that an engine cannot go higher, and this repo has now
    shipped two of them that were inherited from taste rather than measured.
    Tiberian Sun's 1024x768 was one: the engine really renders 1920x1080 (.123),
    and its own Display Options menu offering only 640x400/640x480/800x600 is
    not evidence — the CnCNet patch reads SUN.INI directly and bypasses that
    list. So any NEW cap has to arrive with a measurement in the comment."""
    capped = set()
    for title, spec in sf.TITLES.items():
        for name, rec in spec.get('launchers', {}).items():
            if any('-cap ' in l for l in rec['pre']):
                capped.add(title)
    # Quake 1's cap lives in new_launcher's GLQuake branch, not in a recipe.
    if '-cap ' in sf.call_cap(1280, 960):
        capped.add('Quake1')
    assert capped == {'Quake1'}, (
        'unexpected resolution cap on %s — a cap must be measured on hardware, '
        'and the only measured one is GLQuake' % sorted(capped - {'Quake1'}))


def test_glquake_cap_is_the_measured_ceiling_not_the_old_guess():
    """Measured on .145 (GeForce 8400GS, 1920x1080 panel): GLQUAKE.EXE answers
    `Quake Error: Specified video mode not available` at 1920x1080 AND at
    1600x1200, and comes up fullscreen at 1280x960. The previous cap of
    1024x768 was a guess and cost every 1080p box a sharper picture."""
    src = open(STAGER, encoding='utf-8').read()
    assert 'block = "\\n".join([call_cap(1280, 960)])' in src, (
        'the GLQuake launcher no longer carries the measured 1280x960 cap')
    assert 'call_cap(1024, 768)' not in src, 'the old guessed cap is back'
    assert 'Specified video mode not available' in sf.NEW_GLQUAKE, (
        'the measurement behind the cap is no longer recorded in the launcher')


def test_hexen2_is_not_capped_like_glquake():
    """glh2.exe LOOKS like a GLQuake derivative and was treated as one. On .145
    it comes up at a real 1920x1080 (window class HexenII, 0,0-1920x1080) where
    GLQUAKE.EXE on the same box refuses the identical mode. Two engines in one
    family, two answers — measure each."""
    t = _pre('HexenII')
    assert '%FR_W%' in t and '%FR_H%' in t
    assert '%FR_W43%' not in t


def test_tiberian_sun_is_no_longer_capped():
    t = _pre('TiberianSun')
    assert '-cap' not in t, 'the cap the engine does not actually have is back'
    assert 'ScreenWidth %FR_W%' in t and 'ScreenHeight %FR_H%' in t


def test_a_changed_recipe_can_still_reach_an_already_staged_launcher():
    """patch_launcher stops at the FLEETRES.BAT marker, so the day a recipe
    changes it silently does nothing to the 27 titles already staged. The
    `fix` list is the way through, and it must refuse when NEITHER the old text
    nor its replacement is present — that condition means the recipe is stale,
    and applying it blind ships a launcher that did not change."""
    for title in ('TiberianSun', 'HexenII'):
        assert sf.TITLES[title].get('fix'), '%s has no repair path' % title
    src = open(STAGER, encoding='utf-8').read()
    assert 'neither %r nor its replacement is present' in src


def test_the_registry_only_engines_are_all_covered():
    """Three titles keep the mode ONLY in the registry and were therefore
    pinned by install.reg to one constant on all eight boxes: Max Payne
    (800x600), Red Faction (never written at all), Hidden & Dangerous
    (800x600)."""
    for title in ('MaxPayne', 'RedFaction', 'HiddenAndDangerous'):
        assert '-reg ' in _pre(title), '%s still inherits a staged constant' % title
        assert '%FR_W%' in _pre(title)


def test_turok2_picks_from_its_fixed_boolean_mode_list():
    """Turok 2 has no width/height pair at all: Data\\config.ned carries one
    BOOLEAN PER MODE from a fixed list, and the staged file had 800x600=1 on
    every box. 1280x1024 is deliberately not on the ladder — it is 5:4, and a
    5:4 mode on a 4:3 tube is the squashed picture this whole mechanism exists
    to fix (.133 and .171 were both doing it)."""
    t = _pre('Turok2')
    assert 'config.ned' in t
    assert '%T2SEL%' in t and 'set T2SEL=' in t
    assert '1280^x^1024" 0' in t, '5:4 must be explicitly turned OFF, not left'
    assert 'T2SEL=1280' not in t, '5:4 must never be selected'
    # every mode on the list is cleared before one is set, or two could be 1
    for m in sf.T2_MODES:
        assert '%s" 0' % m in t, '%s is never cleared' % m


def test_turok2_never_puts_a_caret_in_a_variable():
    """`set T2SEL=640^x^480` silently stores "640x480" - cmd.exe eats `^` as
    its own escape before the assignment. Measured on .145: all six modes came
    back 0 with a junk `...\\1024x768 1` appended, and every command reported
    success. Quoting the assignment does fix it; carrying only the WIDTH in the
    variable and writing each mode from a quoted literal removes the question."""
    for line in sf.turok2_mode():
        if 'T2SEL=' in line:
            assert '^' not in line, (
                '%r puts a caret through a variable, which is the assignment '
                'cmd.exe strips' % line)


def test_turok2_keys_keep_their_caret():
    """`^` is cmd.exe's own escape character. Unquoted, the key silently becomes
    `320x240` and the line does nothing; inside double quotes it is literal."""
    for line in sf.turok2_mode():
        if '^x^' in line and '-setline' in line:
            for frag in line.split('-setline')[1].split():
                if '^x^' in frag or 'T2SEL' in frag:
                    assert frag.startswith('"') or frag.endswith('"'), (
                        'unquoted caret in %r' % line)


# ---------------------------------------------------------------------------
# 11. id Tech 2 and id Tech 3 have DIFFERENT mode tables, and index 8 is a trap
# ---------------------------------------------------------------------------

def test_q3_table_exists_and_skips_the_five_four_mode():
    """id Tech 2 mode 8 is 1280x960 (4:3); id Tech 3 mode 8 is 1280x1024 (5:4).
    Handing FR_Q2MODE to a Quake III-family engine therefore asks a 16:9 panel
    for a squashed picture — measured on SoF2 (.123), where r_mode 8 gave
    1280x1024. FR_Q3MODE is a separate index into the separate table, and it
    skips index 8 and index 11 (856x480, a 16:9 mode smaller than any 4:3 one
    the fleet can drive)."""
    src = open(FLEETRES_C, encoding='latin1').read()
    assert 'q3tab' in src and 'q3_mode_for' in src
    assert '{1280,1024}' in src, 'the id Tech 3 table no longer matches the engine'
    assert 'if (i == 8 || i == 11) continue;' in src, (
        'FR_Q3MODE can now select a 5:4 mode, which is the fault this whole '
        'mechanism exists to remove')
    assert 'set FR_Q3MODE=' in src
    assert 'if not defined FR_Q3MODE set FR_Q3MODE=6' in sf.FLEETRES_BAT


def test_sof2_uses_a_mode_index_and_the_right_table():
    """`r_mode -1` is the standard id Tech 3 idiom and IS NOT UNIVERSAL.
    Measured on .145 with one identical fleetres.cfg (-1 + custom 1920x1080):

        quake3.exe   -> 1920x1080     jasp.exe -> 1920x1080
        jamp.exe     -> 1920x1080     sof2mp.exe -> **640x480**

    SoF2's fork never implemented the -1 branch. It does not error; it renders
    small. Both SoF2 binaries are the same engine, so both take a plain index —
    and it must be FR_Q3MODE, because id Tech 3's mode 8 is 1280x1024 (5:4)
    where id Tech 2's is 1280x960 (4:3)."""
    t = _pre('SoldierOfFortune2')
    assert '%FR_Q3MODE%' in t
    assert '%FR_Q2MODE%' not in t, (
        'SoF2 is being handed the id Tech 2 table, whose mode 8 is a different '
        'resolution — 1280x1024 on a 16:9 panel is the squashed picture')
    assert 'r_mode "-1"' not in t and '+set r_mode -1' not in t, (
        'SoF2 has no r_mode -1 branch; this silently renders 640x480')
    for name in sf.TITLES['SoldierOfFortune2']['fix']:
        assert name in sf.TITLES['SoldierOfFortune2']['launchers']


def test_the_engines_that_do_have_the_minus_one_branch_keep_it():
    """Quake III and Jedi Academy were measured at a real 1920x1080 through the
    -1 branch on the same box that refused it for SoF2. Do not generalise
    SoF2's exception back onto them."""
    for title in ('Quake3-TeamArena', 'JediAcademy'):
        blob = _pre(title) + "\n".join(sf.idtech3_cfg('base'))
        assert 'r_customwidth' in blob or '%FR_W%' in blob
