#!/usr/bin/env python3
"""Unreal Gold on a Glide box: GlideDrv is fullscreen-only
(scripts/fleet/stage-fleetres.py ue1_render_device, 2026-09-28).

WHAT HAPPENED. On .124 (Voodoo 5 6000, our h5 glide3x under AmigaMerlin's
Glide2->Glide3 translator) Unreal Gold opened Glide, shut it down, and hung in
its SECOND grSstWinOpen. Read from the staged tree's own binaries:

- WinDrv.dll's WM_KILLFOCUS handler, in fullscreen, calls EndFullscreen, which
  re-creates the render device from Engine.Engine WindowedRenderDevice at the
  WINDOWED size with Fullscreen=0, then minimizes the window.
- the launcher wrote GlideDrv into WindowedRenderDevice (and RenderDevice) as
  well as GameRenderDevice, so that re-creation is a windowed GlideDrv Init:
  GlideDrv's SetRes opens the hardware fullscreen anyway (800x600, 2 buffers),
  from inside the focus change, and returns FALSE unless the board is an
  SST-96 - after which EndFullscreen's check(RenDev) asserts. On .124 it never
  got that far: it hung in that open.
- GlideDrv maps the viewport to a Glide 2 mode by <= tests (512x384, 640x480,
  800x600, 1024x768, 1280x1024, else 1600x1200) and then resizes the viewport
  to the mode it opened. The launcher's 4:3 pair on .124 is 1280x960, which
  has no Glide 2 mode: the game opened 1280x1024 - 5:4 on a 4:3 tube, and the
  desktop's own size, so it went through Glide's 8 bpp DirectDraw detour. The
  run that stayed up was at 1024x768.

These tests run the generated batch lines through a small model of cmd.exe -
including the rule that bit this repo before: %VAR% inside a ( ) block is
expanded when the BLOCK is parsed - and check what lands in Unreal.ini.
"""
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
STAGER = os.path.join(REPO, 'scripts', 'fleet', 'stage-fleetres.py')


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


sf = _load('stage_fleetres_ue1', STAGER)

GLIDE = 'GlideDrv.GlideRenderDevice'
D3D = 'D3DDrv.D3DRenderDevice'
SOFT = 'SoftDrv.SoftwareRenderDevice'

# GlideDrv.dll (Unreal Gold's, staged tree) UGlideRenderDevice::SetRes, from its
# disassembly: the Glide 2 resolution enum for a viewport of x by y
GLIDE2_SIZE = {3: (512, 384), 7: (640, 480), 8: (800, 600), 12: (1024, 768),
               13: (1280, 1024), 14: (1600, 1200)}


def glidedrv_res(x, y):
    if x <= 512:
        return 3
    if x <= 640 and y <= 480:
        return 7
    if x <= 800 and y <= 600:
        return 8
    if x <= 1024 and y <= 768:
        return 12
    if x <= 1280 and y <= 1024:
        return 13
    return 14


# ---------------------------------------------------------------------------
# a model of the cmd.exe the generated lines use
# ---------------------------------------------------------------------------
def _expand(line, env):
    return re.sub(r'%([A-Za-z_][A-Za-z0-9_]*)%', lambda m: env.get(m.group(1).upper(), ''), line)


def _execute(line, env, writes):
    line = line.strip()
    if not line or line.lower().startswith('rem'):
        return
    m = re.match(r'if /i "([^"]*)"=="([^"]*)" (.*)$', line)
    if m:
        if m.group(1).lower() == m.group(2).lower():
            _execute(m.group(3), env, writes)
        return
    m = re.match(r'if (\S+) GEQ (\d+) (.*)$', line)
    if m:
        try:
            ok = int(m.group(1)) >= int(m.group(2))
        except ValueError:
            ok = m.group(1) >= m.group(2)
        if ok:
            _execute(m.group(3), env, writes)
        return
    m = re.match(r'if exist "[^"]*" (.*)$', line)
    if m:
        _execute(m.group(1), env, writes)
        return
    m = re.match(r'set "([A-Za-z_][A-Za-z0-9_]*)=(.*)"$', line) or \
        re.match(r'set ([A-Za-z_][A-Za-z0-9_]*)=(.*)$', line)
    if m:
        env[m.group(1).upper()] = m.group(2)
        return
    m = re.match(r'"[^"]*FLEETRES\.EXE" -ini "[^"]*" (\S+) (\S+) (.*)$', line)
    if m:
        writes[(m.group(1), m.group(2))] = m.group(3)
        return
    raise AssertionError('the cmd model does not know this line: %r' % line)


def run_lines(lines, env):
    """Execute, returning {(section, key): value} of the FLEETRES -ini writes.
    A `(` block is expanded as a whole when it is reached, as cmd.exe does."""
    writes = {}
    i = 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r'if exist "[^"]*" \($', line.strip())
        if m:
            j = i + 1
            body = []
            while lines[j].strip() != ')':
                body.append(lines[j])
                j += 1
            for b in [_expand(x, env) for x in body]:       # parse time
                _execute(b, env, writes)
            i = j + 1
            continue
        _execute(_expand(line, env), env, writes)
        i += 1
    return writes


def ue1_lines():
    """The Play Unreal Gold.bat lines after FLEETRES.BAT: the ue_ini block, then
    the render-device block without its nGlide rename (moves, not ini writes)."""
    pre = sf.ue_ini("System\\Unreal.ini")
    post = sf.TITLES["UnrealGold"]["post"][0]["lines"]
    start = next(i for i, l in enumerate(post) if 'UE1WDEV' in l or 'RenderDevice' in l)
    return pre + post[start:]


def launch(w, h, w43, h43, dev):
    env = {'FR_W': str(w), 'FR_H': str(h), 'FR_W43': str(w43), 'FR_H43': str(h43),
           'FR_UE1DEV': dev}
    return run_lines(ue1_lines(), env)


BOXES = [
    # (why, FR_W, FR_H, FR_W43, FR_H43)
    ('.124 V5 6000, 1280x1024 desktop on a 4:3 CRT', 1280, 960, 1280, 960),
    ('a 1920x1080 LCD', 1920, 1080, 1440, 1080),
    ('a 1600x1200 CRT', 1600, 1200, 1600, 1200),
    ('a Voodoo 2 box capped at 800x600 (ResCapW/H)', 800, 600, 800, 600),
    ('the FLEETRES.BAT fallback', 1024, 768, 1024, 768),
    ('a 640x480 floor', 640, 480, 640, 480),
]


# ---------------------------------------------------------------------------
# the tests
# ---------------------------------------------------------------------------
def test_glidedrv_is_never_the_windowed_render_device():
    for why, w, h, w43, h43 in BOXES:
        out = launch(w, h, w43, h43, GLIDE)
        assert out[('Engine.Engine', 'GameRenderDevice')] == GLIDE, why
        # WM_KILLFOCUS -> EndFullscreen re-creates THIS one, windowed
        assert out[('Engine.Engine', 'WindowedRenderDevice')] == SOFT, why
        assert out[('Engine.Engine', 'RenderDevice')] == SOFT, why


def test_the_generated_lines_never_write_the_game_device_as_the_windowed_one():
    text = "\n".join(sf.TITLES["UnrealGold"]["post"][0]["lines"])
    assert 'WindowedRenderDevice %FR_UE1DEV%' not in text
    assert 'Engine.Engine RenderDevice %FR_UE1DEV%' not in text








def test_a_d3d_box_is_unchanged():
    for why, w, h, w43, h43 in BOXES:
        out = launch(w, h, w43, h43, D3D)
        for key in ('GameRenderDevice', 'WindowedRenderDevice', 'RenderDevice'):
            assert out[('Engine.Engine', key)] == D3D, (why, key)
        # the mode stays FLEETRES' own (ue_ini), not a Glide 2 one
        assert out[('WinDrv.WindowsClient', 'FullscreenViewportX')] == str(w), why
        assert out[('WinDrv.WindowsClient', 'FullscreenViewportY')] == str(h), why


def test_no_block_uses_a_variable_it_sets():
    """cmd.exe expands %VAR% in a ( ) block when the block is PARSED: a set and a
    use in one block see the old value - the render-device block would then
    write an empty or stale device, and every line would return success."""
    lines = sf.TITLES["UnrealGold"]["post"][0]["lines"]
    depth, sets = 0, []
    for line in lines:
        s = line.strip()
        if s.endswith('('):
            depth += 1
            sets = []
            continue
        if s == ')' or s.startswith(') else'):
            depth = max(0, depth - 1)
            continue
        if depth:
            m = re.match(r'(?:if .* )?set "?([A-Za-z_][A-Za-z0-9_]*)=', s)
            if m:
                sets.append(m.group(1).upper())
            for v in re.findall(r'%([A-Za-z_][A-Za-z0-9_]*)%', s):
                assert v.upper() not in sets, (v, s)


def test_the_model_catches_the_old_launcher():
    """The same model run over the block this replaces reports GlideDrv as the
    windowed device - so the tests above can tell them apart."""
    old = sf.ue_ini("System\\Unreal.ini") + [
        'if exist "%~dp0FLEETRES.EXE" (',
        '  "%~dp0FLEETRES.EXE" -ini "%~dp0System\\Unreal.ini" Engine.Engine GameRenderDevice %FR_UE1DEV%',
        '  "%~dp0FLEETRES.EXE" -ini "%~dp0System\\Unreal.ini" Engine.Engine WindowedRenderDevice %FR_UE1DEV%',
        '  "%~dp0FLEETRES.EXE" -ini "%~dp0System\\Unreal.ini" Engine.Engine RenderDevice %FR_UE1DEV%',
        ')',
    ]
    env = {'FR_W': '1280', 'FR_H': '960', 'FR_W43': '1280', 'FR_H43': '960', 'FR_UE1DEV': GLIDE}
    out = run_lines(old, env)
    assert out[('Engine.Engine', 'WindowedRenderDevice')] == GLIDE


def test_the_block_keeps_its_first_line_so_a_stale_copy_is_refreshed():
    """post_block() replaces an older block from its FIRST line to the anchor:
    a changed first line would leave the old block on the share for good."""
    lines = sf.TITLES["UnrealGold"]["post"][0]["lines"]
    assert lines[0] == 'rem ---- per-box RENDER DEVICE (see stage-fleetres.py) --------------'
    assert sf.TITLES["UnrealGold"]["post"][0]["marker"] == "FR_GLIDE"
