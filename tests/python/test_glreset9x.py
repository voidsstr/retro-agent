"""scripts/fleet/win9x/glreset9x.c - give the screen back from a Voodoo 2 a killed Glide game left switched in.

Measured 2026-09-30 in the Win98 build VM: Quake II (3dfx) ended by PROCKILL
never called grSstWinClose/grGlideShutdown, and the passthrough kept showing
its LOADING frame over a healthy desktop. glreset9x opens and closes Glide the
way a well-behaved game does. Pinned here: every open is paired with a close
and a shutdown, the splash is suppressed (a capture during it reads as 'the
desktop'), and the binary imports only what Windows 98 resolves (a single
unresolved import kills a 9x process at load with no message).
"""
import os
import re
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "scripts", "fleet", "win9x", "glreset9x.c")


def test_every_open_is_closed_and_the_splash_is_off():
    s = open(SRC).read()
    assert s.index('SetEnvironmentVariableA("FX_GLIDE_NO_SPLASH", "1")') < s.index('LoadLibraryA("glide2x.dll")')
    body = s[s.index("ok = wopen("):]
    assert re.search(r"wclose\(\);\s*shut\(\);", body), "close then shutdown after a successful open"
    assert re.search(r"if \(!ok\) \{.*?shut\(\);", body, re.S), "a refused open still shuts Glide down"
    for n in ("_grGlideInit@0", "_grGlideShutdown@0", "_grSstWinClose@0", "_grSstWinOpen@28"):
        assert n in s


def test_it_imports_only_kernel32(tmp_path):
    cc, od = shutil.which("i686-w64-mingw32-gcc"), shutil.which("i686-w64-mingw32-objdump")
    if not cc or not od:
        pytest.skip("no mingw - glreset9x was NOT built")
    exe = tmp_path / "g.exe"
    subprocess.run([cc, "-O1", "-march=i586", "-mwindows", "-nostdlib", "-fno-builtin", "-e", "_start@0",
                    "-o", str(exe), SRC, "-lkernel32", "-s"], check=True)
    dump = subprocess.run([od, "-p", str(exe)], capture_output=True, text=True, check=True).stdout
    assert {d.lower() for d in re.findall(r"DLL Name: (\S+)", dump)} == {"kernel32.dll"}
