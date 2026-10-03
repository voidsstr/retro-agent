#!/usr/bin/env python3
"""scripts/benchmarks/d3_timetest.py - Descent 3's own benchmark (retail 1.4
`-timetest <demo>`, the GameGauge mode that writes fps.txt).

The numbers it produced on .124 decided the staged -framecap (see
test_descent3_staging.py), so the way it starts the game is pinned: from the
tree (main.exe reads its data from the working directory - an absolute-path
launch is a different game state), detached, with the previous fps.txt deleted
first so an old result can never be read as the new one, and with --env set
for the GAME only (the agent's own environment is not the desktop's: on .124
it lacks the system FX_GLIDE_SWAPINTERVAL=1, so a run without it is vsync OFF).
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
PATH = os.path.join(REPO, 'scripts', 'benchmarks', 'd3_timetest.py')


def _load():
    spec = importlib.util.spec_from_file_location('d3_timetest', PATH)
    mod = importlib.util.module_from_spec(spec)
    sys.modules['d3_timetest'] = mod
    spec.loader.exec_module(mod)
    return mod


tt = _load()


def test_the_game_starts_from_its_tree_after_the_old_result_is_gone():
    bat, args = tt.launch_bat('Secret2.dem', 1280, 960)
    lines = bat.split('\r\n')
    assert lines[0] == '@echo off'
    cd = lines.index('cd /d "C:\\Games\\Descent3"')
    rm = lines.index('if exist fps.txt del fps.txt')
    st = [i for i, l in enumerate(lines) if l.startswith('start "" main.exe ')]
    assert len(st) == 1 and cd < rm < st[0]
    assert args == '-launched -nointro -timetest Secret2.dem -Width 1280 -Height 960'
    assert '\n' not in bat.replace('\r\n', '')


def test_env_is_set_before_the_start_and_extra_is_appended():
    bat, args = tt.launch_bat('Secret2.dem', 1024, 768,
                              ['FX_GLIDE_SWAPINTERVAL=1'], '-framecap 85')
    lines = bat.split('\r\n')
    st = [i for i, l in enumerate(lines) if l.startswith('start ')][0]
    assert lines.index('set FX_GLIDE_SWAPINTERVAL=1') < st
    assert args.endswith('-Width 1024 -Height 768 -framecap 85')
    assert lines[st] == 'start "" main.exe ' + args
