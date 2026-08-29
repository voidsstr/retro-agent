#!/usr/bin/env python3
"""The refresh deploy script must strip stale cvars in BOTH config dialects.

WHY THIS EXISTS. deploy_game_refresh.py rewrites a managed block into each
game's autoexec and strips stale copies of the same cvars elsewhere in the file,
because a leftover `gl_vsync "0"` further down wins over the block and silently
undoes the setting. That strip worked for Quake-family configs and silently did
nothing for GoldSrc.

The two sides of the comparison extracted the cvar name differently. The managed
set used split()[1] unconditionally - correct for `seta r_swapInterval "0"`,
where token 1 is the name, and wrong for the GoldSrc form `gl_vsync "1"`, where
token 1 is the VALUE. So managed came out as {'"1"', '"100"'}: a set no cvar
name can ever match. Nothing was stripped, in exactly the case the code was
written to handle.

Both sides now go through cvar_name(). These cases assert the fixed behaviour
AND the old broken one, so a regression to split()[1] fails here.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, '..', '..', 'scripts', 'game-refresh',
                      'deploy_game_refresh.py')

spec = importlib.util.spec_from_file_location('dgr', SCRIPT)
dgr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dgr)


def test_cvar_name_quake_dialect():
    assert dgr.cvar_name('seta r_swapInterval "0"') == 'r_swapInterval'
    assert dgr.cvar_name('set cl_maxfps "125"') == 'cl_maxfps'


def test_cvar_name_goldsrc_dialect():
    # No verb. The name is token 0, and reading token 1 here is the bug.
    assert dgr.cvar_name('gl_vsync "1"') == 'gl_vsync'
    assert dgr.cvar_name('fps_max "100"') == 'fps_max'
    assert dgr.cvar_name('gl_vsync "1"') != '"1"'


def test_cvar_name_degenerate():
    assert dgr.cvar_name('') == ''
    assert dgr.cvar_name('   ') == ''
    assert dgr.cvar_name('set') == 'set'          # verb with no name


def test_goldsrc_stale_cvar_is_stripped():
    """The regression itself: a stale gl_vsync must not survive the merge."""
    existing = '\n'.join([
        'hud_fastswitch 1',
        'gl_vsync "0"',          # stale - must be stripped
        'fps_max "60"',          # stale - must be stripped
        'name "player"',
    ])
    block = ['gl_vsync "1"', 'fps_max "100"']
    out = dgr.merge_block(existing, block, '//')

    body = out.split(dgr.BLOCK_START)[0]
    assert 'gl_vsync "0"' not in body, 'stale gl_vsync survived - the old bug'
    assert 'fps_max "60"' not in body, 'stale fps_max survived - the old bug'
    # untouched lines stay
    assert 'hud_fastswitch 1' in body
    assert 'name "player"' in body
    # and the managed block is present with the values we want
    assert 'gl_vsync "1"' in out
    assert 'fps_max "100"' in out


def test_quake_stale_cvar_still_stripped():
    """The dialect that already worked must keep working."""
    existing = '\n'.join([
        'seta cl_run "1"',
        'seta r_swapInterval "1"',   # stale - must be stripped
        'bind w +forward',
    ])
    block = ['seta r_swapInterval "0"']
    out = dgr.merge_block(existing, block, '//')

    body = out.split(dgr.BLOCK_START)[0]
    assert 'seta r_swapInterval "1"' not in body
    assert 'seta cl_run "1"' in body
    assert 'bind w +forward' in body
    assert 'seta r_swapInterval "0"' in out


def test_merge_is_idempotent():
    """Running the deploy twice must not stack blocks or lose user lines."""
    existing = 'hud_fastswitch 1'
    block = ['gl_vsync "1"']
    once = dgr.merge_block(existing, block, '//')
    twice = dgr.merge_block(once, block, '//')
    assert twice.count(dgr.BLOCK_START) == 1, 'a second run stacked a block'
    assert 'hud_fastswitch 1' in twice
