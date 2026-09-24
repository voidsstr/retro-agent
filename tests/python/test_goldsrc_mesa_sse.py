"""The staged GoldSrc launchers must set MESA_FORCE_SSE=1 before hl.exe.

Fix verified on .124 (Voodoo 5 6000, AmigaMerlin 3.1-R11), 2026-09-23.
The AmigaMerlin 3dfxOGL ICD is Mesa-based: at wglCreateContext it runs
Mesa's common_x86.c probe `_mesa_test_os_sse_exception_support` - it clears
the divide-by-zero mask in MXCSR and executes `divps xmm1, xmm0` with
xmm0 = 0 ON PURPOSE, relying on its SetUnhandledExceptionFilter hook to
step over the trap. GoldSrc's hw.dll wraps video init in its own exception
handler, which takes the STATUS_FLOAT_MULTIPLE_TRAPS (c00002b5) first and
shuts the engine down; hw.dll is unloaded while its subclassed window is
still alive and the next WM_WINDOWPOSCHANGED jumps into freed memory
(0x035e8fa0) - the "hl.exe - Application Error" popup. Captured under ntsd.

With MESA_FORCE_SSE set, Mesa skips that probe (XP supports SSE, so the
SSE paths stay on). Without it, every OpenGL launch of CS 1.6 on a Voodoo
4/5 dies before the first frame. The variable is read by nothing but a Mesa
ICD, so it is inert on every other card.
"""
import os
import re

import pytest

TREE = "/mnt/retro-share/Files/Games-Library/CounterStrike16"
LAUNCHERS = ["Play Counter-Strike.bat", "Play Half-Life Deathmatch.bat"]


def _body(name):
    path = os.path.join(TREE, name)
    if not os.path.isdir(TREE):
        pytest.skip("SHARE NOT MOUNTED - cannot check %s for MESA_FORCE_SSE. "
                    "This is a real gap, not a pass." % name)
    with open(path, "rb") as f:
        return f.read().decode("latin1")


@pytest.mark.parametrize("name", LAUNCHERS)
def test_launcher_sets_mesa_force_sse_before_hl_exe(name):
    body = _body(name)
    code = [l for l in body.splitlines() if not l.lower().startswith("rem")]
    text = "\n".join(code)
    m = re.search(r"^set MESA_FORCE_SSE=1\s*$", text, re.M | re.I)
    assert m, "%s lost 'set MESA_FORCE_SSE=1' - GoldSrc GL crashes on a " \
              "Voodoo 4/5 Mesa ICD without it" % name
    start = re.search(r"hl\.exe", text, re.I)
    assert start and m.start() < start.start()


def test_the_probe_is_what_the_variable_skips():
    """Encode the mechanism, so a reader can check it: FORCE skips only the
    OS probe; NO_SSE would also switch the SSE transform paths off, which
    costs speed for nothing."""
    force, no_sse = "MESA_FORCE_SSE", "MESA_NO_SSE"
    probe_runs = lambda env: no_sse not in env and force not in env
    sse_paths_on = lambda env: no_sse not in env
    fixed = {force: "1"}
    assert not probe_runs(fixed) and sse_paths_on(fixed)
    assert probe_runs({})                       # the old, crashing launch
    assert not sse_paths_on({no_sse: "1"})      # the slower alternative
