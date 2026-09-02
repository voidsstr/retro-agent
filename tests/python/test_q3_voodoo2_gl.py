#!/usr/bin/env python3
"""Quake III on a 3dfx Voodoo 2 box: the engine choice is per box, and it is
the RETAIL engine or nothing.

WHY THIS EXISTS. On .171 Quake III had been rendering on the box's Intel 865G
with the Voodoo 2 sitting idle, and nothing said so — the game ran, fullscreen,
at the right resolution. A Voodoo 2 is Class=MEDIA: it does not drive the
desktop and it registers NO OpenGL ICD, so opengl32.dll can never select it.
The card's driver ships a STANDALONE OpenGL, 3dfxvgl.dll, which the retail id
engine loads when r_glDriver names it (GL_RENDERER then reads
"3Dfx/Voodoo2/2 TMUs/4 MB/stand-alone").

THE PART THAT IS EXPENSIVE TO RE-DERIVE: ioquake3 cannot be pointed at that
card at all. Three routes were measured on .171 on 2026-09-01 and all three
failed, each one silently falling back to the Intel chip:

  * r_glDriver — ioquake3 dropped the cvar. `seta r_glDriver "3dfxvgl"` in the
    config is simply ignored; the log shows GL_RENDERER: Intel 865G.
  * a game-local opengl32.dll — opengl32 is in XP's KnownDLLs, so the
    application directory never wins. Copying 3dfxvgl.dll in as opengl32.dll
    changed nothing.
  * SDL_VIDEO_GL_DRIVER — the bundled SDL.dll hardcodes "OPENGL32.DLL" and
    carries no such string at all (`strings SDL.dll` finds only OPENGL32.DLL).

So the staged tree selects the engine per box, in FLEETGL.BAT, keyed on
3dfxvgl.dll existing in system32 — which is true only where the Voodoo 2 driver
is installed, so every other machine keeps ioquake3 unchanged. This does NOT
cost multiplayer: both fleet Q3 servers run com_legacyprotocol 68, which retail
1.32c speaks.

The regression this guards is a quiet one: someone "simplifying" the launchers
back to a hardcoded ioquake3.x86.exe returns .171 to the Intel chip, and the
game still runs, so nothing looks broken.

It SKIPS loudly when the share is not mounted.
"""
import os
import sys

import pytest

LIB = "/mnt/retro-share/Files/Games-Library/Quake3-TeamArena"
LAUNCHERS = ("Play Quake III Arena.bat",
             "Play Team Arena.bat",
             "Play Quake III Arena - retail 1.32c.bat")


def main():
    print("== Quake III picks the Voodoo 2 per box (FLEETGL.BAT) ==")
    if not os.path.isdir(LIB):
        print("  SKIP  %s not mounted - Quake III's GL selection was NOT" % LIB)
        print("        checked. A .171 regression here is invisible: the game")
        print("        still runs, just on the Intel chip.")
        return 0

    fails = []
    gl = os.path.join(LIB, "FLEETGL.BAT")
    if not os.path.isfile(gl):
        print("  FAIL  FLEETGL.BAT is missing - the tree cannot select the")
        print("        Voodoo 2 and .171 renders on the Intel 865G")
        return 1

    body = open(gl, "r", errors="replace").read()
    for need, why in (
        ("3dfxvgl", "the standalone Voodoo 2 OpenGL is never named"),
        ("r_glDriver", "the retail engine is never told which GL to load"),
        ("quake3.exe", "the Voodoo 2 branch does not select the retail engine"),
        ('r_colorbits "16"', "a Voodoo 2 has no 32-bit mode"),
    ):
        if need not in body:
            fails.append("FLEETGL.BAT does not mention %s - %s" % (need, why))

    # The Voodoo 2 branch must key on a file that exists ONLY on such a box,
    # or the other seven machines get an engine that cannot join the server.
    if "%SystemRoot%\\system32\\3dfxvgl.dll" not in body:
        fails.append("FLEETGL.BAT does not gate on system32\\3dfxvgl.dll - "
                     "the per-box test is what keeps the other seven boxes on "
                     "ioquake3")

    for name in LAUNCHERS:
        p = os.path.join(LIB, name)
        if not os.path.isfile(p):
            fails.append("%s is missing" % name)
            continue
        d = open(p, "r", errors="replace").read()
        if "FLEETGL.BAT" not in d:
            fails.append("%s never calls FLEETGL.BAT - on .171 it will render "
                         "on the Intel 865G and look fine" % name)
        if name != LAUNCHERS[2] and "ioquake3.x86.exe" in d:
            fails.append("%s starts ioquake3.x86.exe directly instead of "
                         "%%FR_ENGINE%% - ioquake3 CANNOT reach the Voodoo 2 "
                         "(no r_glDriver, KnownDLLs, SDL hardcodes "
                         "OPENGL32.DLL)" % name)

    for f in fails:
        print("  FAIL  %s" % f)
    if fails:
        return 1
    print("  ok    FLEETGL.BAT selects 3dfxvgl + the retail engine on a Voodoo")
    print("        box; all three launchers call it")
    return 0


def test_q3_selects_the_voodoo2_per_box():
    """pytest wrapper - tests/python is what tests/run_all.sh collects."""
    if not os.path.isdir(LIB):
        pytest.skip("%s not mounted - Quake III's GL selection was NOT "
                    "checked; a .171 regression here is invisible because "
                    "the game still runs, just on the Intel chip" % LIB)
    assert main() == 0


if __name__ == "__main__":
    sys.exit(main())
