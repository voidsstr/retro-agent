"""Regression: game_sweep.py must never sweep a DRIVER PACKAGE directory.

2026-09-01, box .185 (Voodoo 5 6000): the sweep's filename scan found
C:\\DRIVERS\\amigamerlin-3.1-R11\\{3dfxOGL,glide2x,glide3x}.dll and planned to
replace/retire them. That tree is not a game -- it is the installer you roll
back TO if our driver goes wrong, and rewriting it removes the safety net
silently. Fix: SKIP_DIRS (always applied) plus a --exclude escape hatch.
"""
import asyncio
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SWEEP = os.path.abspath(os.path.join(HERE, "..", "..", ".claude", "skills",
                                     "driver-install", "game_sweep.py"))

FAKE_DIR = "\n".join([
    r"C:\Games\Quake2Complete\3dfxgl.dll",
    r"C:\Games\RedAlert2\ddraw.dll",
    r"C:\DRIVERS\amigamerlin-3.1-R11\3dfxOGL.dll",   # driver package - must skip
    r"C:\DRIVERS\amigamerlin-3.1-R11\glide2x.dll",   # driver package - must skip
    r"C:\WINDOWS\system32\opengl32.dll",             # system - must skip
    r"C:\RETRO_AGENT\3dfx-v56k\glide3x.dll",         # our staging - must skip
    r"C:\NVIDIA\foo\opengl32.dll",                   # vendor package - must skip
    r"C:\Installers\SomeGame\glide2x.dll",           # only skipped via --exclude
])


def _mod():
    spec = importlib.util.spec_from_file_location("game_sweep", SWEEP)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["game_sweep"] = mod
    spec.loader.exec_module(mod)
    return mod


def _scan(mod, exclude=()):
    async def fake_rc(_c, _cmd, t=60):
        return FAKE_DIR
    mod.rc = fake_rc
    return asyncio.get_event_loop_policy().new_event_loop().run_until_complete(
        mod.scan(object(), ["C"], exclude))


def test_driver_package_and_system_dirs_are_never_swept():
    mod = _mod()
    got = _scan(mod)
    assert r"C:\Games\Quake2Complete\3dfxgl.dll" in got
    assert r"C:\Games\RedAlert2\ddraw.dll" in got
    for skipped in (r"C:\DRIVERS\amigamerlin-3.1-R11\3dfxOGL.dll",
                    r"C:\DRIVERS\amigamerlin-3.1-R11\glide2x.dll",
                    r"C:\WINDOWS\system32\opengl32.dll",
                    r"C:\RETRO_AGENT\3dfx-v56k\glide3x.dll",
                    r"C:\NVIDIA\foo\opengl32.dll"):
        assert skipped not in got, "swept a directory that must be left alone: " + skipped


def test_exclude_adds_skips_and_is_case_insensitive():
    mod = _mod()
    assert r"C:\Installers\SomeGame\glide2x.dll" in _scan(mod)
    assert r"C:\Installers\SomeGame\glide2x.dll" not in _scan(mod, [r"\installers\\"[:-1]])


def test_skip_dirs_still_applies_when_exclude_is_given():
    mod = _mod()
    got = _scan(mod, [r"\nothing-matches\\"[:-1]])
    assert r"C:\DRIVERS\amigamerlin-3.1-R11\3dfxOGL.dll" not in got
