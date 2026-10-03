#!/usr/bin/env python3
"""Descent 3 staging: the user's controls, per-box renderer and frame cap.

WHY THIS EXISTS (2026-10-02). The user set up their keyboard and mouse (WASD +
F, mouse) in Descent 3 on .124 and asked for exactly that to be on every box,
automatically. Descent 3 keeps the whole control mapping IN THE PILOT FILE
(sdf.plt), so the pilot from .124 is staged in the tree and install.reg makes it
the Default_pilot - a fresh box starts on it with no pilot prompt.

Three things the same session found, each pinned here:

* PredefDetailSetting must NOT be in install.reg. main.exe reads the per-option
  detail values and THEN applies this preset (0-3) over all of them; 4 =
  custom keeps them (init.cpp LoadGameSettings, released source). The staged 1
  reset every box's own detail at each GAMESYNC - .124's maxed custom detail
  went to medium. Absent, main.exe defaults to medium: what the 1 gave anyway.
* PreferredRenderer 2 (OpenGL through the 3dfx ICD) where the 3dfx card drives
  the screen, set by the launcher at every launch, because GAMESYNC re-merges
  install.reg's 3 (Direct3D) at every sync.
* -framecap <monitor rate>: main.exe caps itself at 60 fps (16 ms), which under
  vsync on a 75-100 Hz CRT presents frames at uneven one/two-refresh steps.
  .124, -timetest Secret2.dem, 1280x960, vsync 85 Hz: 60.2 fps stock, 63.9 at
  -framecap 85, 79.0 with vsync off.

The block's logic is executed in a tiny model of the cmd.exe constructs it uses,
over every state a box can be in; a line the model does not know fails the test
instead of being skipped. The share-side checks skip LOUDLY when the library is
not mounted.
"""
import importlib.util
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
STAGER = os.path.join(REPO, 'scripts', 'fleet', 'stage-fleetres.py')
LIB = os.environ.get('RETRO_LIBRARY', '/mnt/retro-share/Files/Games-Library')
D3 = os.path.join(LIB, 'Descent3')


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


sf = _load('stage_fleetres_d3', STAGER)
GLIDE = sf.UE1_GLIDE_DEV


def _block(launcher='Play Descent 3.bat'):
    pbs = [pb for pb in sf.TITLES['Descent3'].get('post', [])
           if pb.get('marker') == 'D3_RENDERER' and pb.get('file') == launcher]
    assert len(pbs) == 1, 'Descent3 lost the D3_RENDERER block of %s' % launcher
    return pbs[0]


# ---------------------------------------------------------------------------
# a model of exactly the cmd.exe constructs the block uses
# ---------------------------------------------------------------------------
def _expand(line, env):
    return re.sub(r'%([A-Za-z0-9_]+)%', lambda m: env.get(m.group(1), ''), line)


def run_block(env_in):
    """Execute the block for one box. Returns (env, renderer_written)."""
    env = {k: v for k, v in env_in.items() if v is not None}
    renderer = None
    for raw in _block()['lines']:
        line = raw.strip()
        if not line or line.lower().startswith('rem '):
            continue
        m = re.fullmatch(r'set /a (\w+)=(\w+)\+0 >nul 2>nul', line)
        if m:
            src = env.get(m.group(2), '')
            if src == '':
                env[m.group(1)] = '0'          # set /a reads empty/undefined as 0
            elif re.fullmatch(r'\d+', src):
                env[m.group(1)] = str(int(src))
            # anything else: set /a errors and the variable keeps its value
            continue
        m = re.fullmatch(r'set (\w+)=(.*)', line)
        if m:
            v = _expand(m.group(2), env)
            if v == '':
                env.pop(m.group(1), None)      # `set X=` undefines X
            else:
                env[m.group(1)] = v
            continue
        x = _expand(line, env)
        m = re.fullmatch(r'if /i "([^"]*)"=="([^"]*)" (reg add .*)', x)
        if m:
            if m.group(1).lower() == m.group(2).lower():
                r = re.search(r'/v PreferredRenderer /t REG_DWORD /d (\d+) /f', m.group(3))
                assert r and 'Outrage\\Descent3' in m.group(3), m.group(3)
                renderer = int(r.group(1))
            continue
        m = re.fullmatch(r'if (\S+) GTR (\d+) set (\w+)=(.*)', x)
        if m:
            # cmd parses the WHOLE line first: an empty left operand is a
            # syntax error that ends the batch file - the game never starts
            assert re.fullmatch(r'\d+', m.group(1)), 'IF on a non-number: %r' % x
            if int(m.group(1)) > int(m.group(2)):
                env[m.group(3)] = m.group(4)
            continue
        if re.fullmatch(r'if\s+GTR.*', x):
            raise AssertionError('empty IF operand - a syntax error: %r' % x)
        raise AssertionError('a line the model does not know: %r' % raw)
    return env, renderer


@pytest.mark.parametrize('dev,want', [
    (GLIDE, 2), (GLIDE.lower(), 2), ('D3DDrv.D3DRenderDevice', None),
    ('OpenGLDrv.OpenGLRenderDevice', None), (None, None), ('', None)])
def test_opengl_only_where_the_3dfx_card_drives_the_screen(dev, want):
    _, renderer = run_block({'FR_UE1DEV': dev, 'FR_HZ': '85'})
    assert renderer == want


@pytest.mark.parametrize('hz,cap', [
    ('85', '-framecap 85'), ('100', '-framecap 100'), ('75', '-framecap 75'),
    ('120', '-framecap 120'), ('60', None), ('59', None), ('0', None),
    ('', None), (None, None), ('85Hz', None)])
def test_frame_cap_is_the_monitor_rate_and_never_breaks_the_batch(hz, cap):
    env, _ = run_block({'FR_UE1DEV': GLIDE, 'FR_HZ': hz})
    assert env.get('D3CAP') == cap


def test_a_stale_cap_from_the_parent_shell_is_cleared():
    env, _ = run_block({'FR_HZ': '60', 'D3CAP': '-framecap 999', 'D3HZ': '999'})
    assert 'D3CAP' not in env


def test_the_block_is_plain_cmd():
    """No ( ... ) group anywhere, so no expanded value can close one early
    (CLAUDE.md), and no delayed expansion. The first rem line's "(see
    stage-fleetres.py)" is a comment outside any group - and post_block
    finds an older copy of the block by that exact line, so it stays."""
    for line in _block()['lines']:
        assert '!' not in line, line
        if line.lower().startswith('rem '):
            continue
        assert '(' not in line and ')' not in line, line


@pytest.mark.parametrize('fix', ['D3_START_FIX', 'D3_JOIN_FIX'])
def test_the_start_lines_take_the_cap_and_the_pilot(fix):
    pairs = getattr(sf, fix)
    assert len(pairs) == 1
    old, new = pairs[0]
    assert old not in new, 'repair() would apply an OLD that is inside NEW forever'
    line = new.rstrip('\r\n')
    assert line.endswith('%D3CAP%') and ' -pilot SDF ' in line
    rec = sf.TITLES['Descent3']['launchers']['Play Descent 3.bat']
    assert rec['sub'][1] == sf.D3_START_FINAL
    assert sf.TITLES['Descent3']['fix'] == {'Play Descent 3.bat': sf.D3_START_FIX,
                                            'Join Descent 3 - LAN.bat': sf.D3_JOIN_FIX}


def test_both_player_launchers_carry_the_block_before_their_start_line():
    posts = {pb['file']: pb for pb in sf.TITLES['Descent3']['post']}
    assert set(posts) == {'Play Descent 3.bat', 'Join Descent 3 - LAN.bat'}
    assert posts['Play Descent 3.bat']['before'] in sf.D3_START_FINAL
    assert posts['Join Descent 3 - LAN.bat']['before'] in sf.D3_JOIN_FIX[0][1]
    for pb in posts.values():
        assert pb['marker'] == 'D3_RENDERER' and pb['lines'] == sf.d3_renderer()


ORIGINAL = ('@echo off\r\n'
            'cd /d "%~dp0"\r\n'
            'start "" "%~dp0main.exe" -launched\r\n')
# the library's Play launcher as staged at 21:53 on 2026-10-02 (cap, no -pilot)
STAGED_WITH_THE_CAP = ('@echo off\r\n'
                       'cd /d "%~dp0"\r\n'
                       'call "%~dp0FLEETRES.BAT"\r\n'
                       + '\r\n'.join(sf.d3_renderer()) + '\r\n'
                       '\r\n'
                       'start "" "%~dp0main.exe" -launched -Width %FR_W% -Height %FR_H% %D3CAP%\r\n')
JOIN = ('@echo off\r\nsetlocal\r\nset "HOSTIP=%~1"\r\n'
        'if not defined HOSTIP set "HOSTIP=192.168.1.132"\r\n'
        'cd /d "%~dp0"\r\ncall "%~dp0FLEETRES.BAT"\r\n'
        'echo  Joining Descent 3 on %HOSTIP% ...\r\n'
        'start "" "%~dp0main.exe" -launched -nointro -pilot SDF -directip +connect %HOSTIP% '
        '-Width %FR_W% -Height %FR_H%\r\nendlocal\r\nexit\r\n')


def _stage(tmp_path, body, join=JOIN):
    t = tmp_path / 'Descent3'
    t.mkdir(exist_ok=True)
    (t / 'Play Descent 3.bat').write_bytes(body.encode('latin1'))
    (t / 'Join Descent 3 - LAN.bat').write_bytes(join.encode('latin1'))
    spec = sf.TITLES['Descent3']
    r = sf.Runner(str(tmp_path), False, False)
    for name, rc in spec['launchers'].items():
        r.patch_launcher(str(t), 'Descent3', name, rc)
    for pb in spec['post']:
        r.post_block(str(t), 'Descent3', pb)
    for name, pairs in spec['fix'].items():
        r.repair(str(t), 'Descent3', name, pairs)
    assert not r.errors, r.errors
    return (t / 'Play Descent 3.bat').read_bytes().decode('latin1')


@pytest.mark.parametrize('start', [ORIGINAL, STAGED_WITH_THE_CAP])
def test_staging_converges_from_every_earlier_launcher(tmp_path, start):
    once = _stage(tmp_path, start)
    assert once.count('call "%~dp0FLEETRES.BAT"') == 1
    assert once.count(_block()['lines'][0]) == 1
    starts = [l for l in once.split('\r\n') if l.startswith('start ')]
    assert starts == [sf.D3_START_FINAL], starts
    assert once.index('set /a D3HZ') < once.index(starts[0])
    assert once.index('call "%~dp0FLEETRES.BAT"') < once.index('set /a D3HZ')
    assert '\n' not in once.replace('\r\n', '')
    join = (tmp_path / 'Descent3' / 'Join Descent 3 - LAN.bat').read_bytes().decode('latin1')
    jstarts = [l for l in join.split('\r\n') if l.startswith('start ')]
    assert jstarts == [sf.D3_JOIN_FIX[0][1].rstrip('\r\n')], jstarts
    assert join.index('call "%~dp0FLEETRES.BAT"') < join.index('set /a D3HZ') < join.index(jstarts[0])
    assert _stage(tmp_path, once, join) == once      # a re-run changes nothing
    assert (tmp_path / 'Descent3' / 'Join Descent 3 - LAN.bat').read_bytes().decode('latin1') == join


# ---------------------------------------------------------------------------
# the library itself (skips LOUDLY when the share is absent)
# ---------------------------------------------------------------------------
def _lib(name):
    if not os.path.isdir(D3):
        pytest.skip('library not mounted at %s - the share-side Descent 3 checks '
                    'did NOT run; this is not a pass' % LIB)
    for n in os.listdir(D3):
        if n.lower() == name.lower():
            return os.path.join(D3, n)
    return None


def _reg_values():
    path = _lib('install.reg')
    assert path, 'Descent3 has no install.reg'
    text = open(path, 'rb').read().decode('latin1')
    assert text.startswith('REGEDIT4'), 'REGEDIT4 merges everywhere; v5 does not on 9x'
    vals = {}
    for line in text.splitlines():
        m = re.match(r'"([^"]+)"=(.*)$', line.strip())
        if m:
            vals[m.group(1).lower()] = m.group(2)
    return vals


def test_the_library_starts_every_box_on_the_staged_pilot():
    vals = _reg_values()
    assert vals.get('default_pilot') == '"sdf.plt"', vals.get('default_pilot')
    plt = _lib('sdf.plt')
    assert plt, 'install.reg names sdf.plt but the tree does not ship it'
    data = open(plt, 'rb').read()
    # the user's pilot from .124 carries its ship and missions by name
    assert b'Pyro-GL' in data and len(data) > 1000


def test_the_library_never_resets_a_boxs_detail_choice():
    vals = _reg_values()
    assert 'predefdetailsetting' not in vals, (
        'install.reg sets PredefDetailSetting again: every GAMESYNC then '
        'overwrites each box\'s own detail with that preset')
    assert vals.get('detaillevelconfigured') == 'dword:00000001'


@pytest.mark.parametrize('name,fix', [('Play Descent 3.bat', 'D3_START_FIX'),
                                      ('Join Descent 3 - LAN.bat', 'D3_JOIN_FIX')])
def test_the_library_launchers_carry_the_block_the_cap_and_the_pilot(name, fix):
    path = _lib(name)
    assert path, name
    body = open(path, 'rb').read().decode('latin1')
    for line in _block()['lines']:
        assert line in body, line
    assert getattr(sf, fix)[0][1] in body
