"""glide2probe - the on-box repro of Unreal Gold's Glide re-open (2026-09-28).

Unreal Gold on .124 (V5 6000, our h5 glide3x under AmigaMerlin's Glide2->Glide3
translator) hung in its SECOND grSstWinOpen. voodoo-cleanroom/tools/glide2probe.c
replays what the game does, read from its own GlideDrv.dll / WinDrv.dll:
grGlideInit once, UE1's re-Init calls between a close and the next open, and the
re-open made from INSIDE the window's WM_KILLFOCUS (WinDrv's EndFullscreen with
GlideDrv as the windowed device). What these tests pin:

- the --ue1 preset is the game's call, not a guess: 13:0 with 3+1 buffers and
  the ABGR colour format first, then 8:0 with 2+1 (the windowed size) - and the
  header says Unreal Gold's "Res=%i Ref=%i" log line prints the two SWAPPED;
- every grSstWinOpen / grSstWinClose / grGlideShutdown-with-a-context goes
  through vcr_pace.h (the CRT rule), each switch recorded whatever it returned;
- the watchdog takes no lock while the main thread may hold one: it copies the
  suspended thread's context and stack and describes them only after
  ResumeThread, names modules from their export directory (never
  GetModuleFileName: the loader lock), reads window titles with
  InternalGetWindowText (never GetWindowText: a message to a hung thread);
- the hard timeout stamps the pace gate AHEAD and ends the process with
  TerminateProcess (ExitProcess would run glide3x's DLL detach beside a hung
  main thread);
- the tool and the stub glide2x compile (i686 mingw, when present).

The off-box run of the whole harness is tools/glide2probe_wine.sh (Wine in the
local retro-wine container against tools/glide2stub.c); it takes minutes, so it
runs here only with GLIDE2PROBE_WINE=1.
"""
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
CR = REPO / "voodoo-cleanroom"
PROBE = (CR / "tools" / "glide2probe.c").read_text()
STUB = CR / "tools" / "glide2stub.c"
CC = shutil.which("i686-w64-mingw32-gcc")


def _blank_comments(src):
    return re.sub(r"/\*.*?\*/", lambda m: " " * len(m.group(0)), src, flags=re.S)


CODE = _blank_comments(PROBE)


def _body(name):
    m = re.search(r"\n(?:static )?[\w *]+?\b%s\s*\([^;{]*\)\s*\{" % re.escape(name), CODE)
    assert m, name
    i, depth = m.end(), 1
    while depth:
        depth += {"{": 1, "}": -1}.get(CODE[i], 0)
        i += 1
    return CODE[m.start():i]


def test_the_ue1_preset_is_what_the_game_calls():
    body = CODE[CODE.index('"--ue1"'):]
    body = body[:body.index("} else {")]
    assert re.search(r"first\s*=\s*\{\s*13,\s*0,\s*3,\s*1\s*\}", body), "first open 13:0 3+1"
    assert re.search(r"second\s*=\s*\{\s*8,\s*0,\s*2,\s*1\s*\}", body), "re-open 8:0 2+1"
    assert "O.cformat = 1;" in body, "UE1 asks for GR_COLORFORMAT_ABGR"
    assert 'O.killfocus = "natural"' in body
    # the swap in Unreal Gold's own log line is written down where it is used
    assert '"Res=0 Ref=13" is' in PROBE and "SWAPPED" in PROBE


def test_the_reopen_is_made_inside_wm_killfocus_and_then_minimized():
    wp = _body("wndproc")
    assert re.search(r"m == WM_KILLFOCUS\)\s*\{[^}]*reopen_in_killfocus\(\)", wp, re.S)
    rk = _body("reopen_in_killfocus")
    # UE1's EndFullscreen: Exit (close), the re-Init calls, the windowed open, minimize
    order = [rk.index(k) for k in ("do_close(", "ue1_requery()", "do_open(",
                                   "SW_SHOWMINNOACTIVE")]
    assert order == sorted(order)


def test_ue1_requery_has_no_grglideinit():
    rq = _body("ue1_requery")
    assert "g_init" not in rq, "UE1 calls grGlideInit once per process"
    for fn in ("g_ver", "g_boards", "g_errcb", "g_query", "g_sel(0)"):
        assert fn in rq, fn


def test_every_switch_is_paced_and_recorded_whatever_it_returned():
    op = _body("do_open")
    a, b = op.index("vcr_pace_before_switch()"), op.index("g_open(")
    c = op.index("vcr_pace_after_switch()")
    assert a < b < c
    between = op[op.index(";", b) + 1:c]
    assert not re.search(r"\breturn\b|\bif\b", between), "recorded only on one outcome"
    cl = _body("do_close")
    assert cl.index("vcr_pace_before_switch()") < cl.index("g_close()") < \
        cl.index("vcr_pace_after_restore()")
    # the only calls of the switch entry points are those
    assert len(re.findall(r"\bg_open\s*\(", CODE)) == 1
    assert len(re.findall(r"\bg_close\s*\(", CODE)) == 1
    main = _body("main")
    shut = main[main.index("int held = g_ctx;"):]
    assert shut.index("vcr_pace_before_switch()") < shut.index("g_shut()") < \
        shut.index("vcr_pace_after_restore()")
    assert '#include "../vcr-kmd/tools/vcr_pace.h"' in PROBE


def test_a_focus_loss_with_a_context_open_is_stamped():
    wp = _body("wndproc")
    assert re.search(r"WM_ACTIVATEAPP && !w\)\s*\{.*?if \(g_ctx\)\s*vcr_pace_mark\(\);", wp, re.S)


def test_the_watchdog_takes_no_lock_while_the_main_thread_is_stuck():
    dump = _body("dump_main_thread")
    # from the successful suspend to the resume (the failed-suspend branch logs:
    # nothing is frozen there)
    s, r = dump.index("memset(&c"), dump.index("ResumeThread(")
    assert dump.index("SuspendThread(") < s
    frozen = dump[s:r]
    # while suspended: only the context and a raw copy of the stack
    assert "GetThreadContext(" in frozen and "ReadProcessMemory(" in frozen
    for bad in ("say(", "describe(", "printf", "GetModuleFileName", "VirtualQuery(", "malloc"):
        if bad == "VirtualQuery(":
            continue    # a syscall, no user-mode lock
        assert bad not in frozen, bad
    assert dump.index("describe(", r) > r
    for fn in ("describe", "foreground_line", "dump_main_thread", "watchdog"):
        b = _body(fn)
        assert "GetModuleFileName" not in b, fn
        assert not re.search(r"\bGetWindowText[AW]?\s*\(", b), fn
    assert "InternalGetWindowText" in _body("foreground_line")


def test_the_hard_timeout_stamps_ahead_then_terminates():
    wd = _body("watchdog")
    assert wd.index("vcr_pace_mark_ahead(VCR_PACE_EXIT_LAG_MS)") < \
        wd.index("TerminateProcess(GetCurrentProcess()")
    assert "ExitProcess" not in wd
    # and it never kills unless asked
    assert re.search(r"if \(O\.hard_s > 0 && g_hung", wd)


def test_the_glide_environment_is_set_before_the_dll_loads():
    main = _body("main")
    load = main.index("LoadLibraryA(O.dll)")
    for kv in ('"FX_GLIDE_NO_PLUGIN=1"', '"FX_GLIDE_ALT_TAB_FIX=0"', '"FX_GLIDE_TRACE=%d"'):
        assert -1 < main.index(kv) < load, kv


@pytest.mark.skipif(CC is None, reason="i686-w64-mingw32-gcc absent - glide2probe NOT compiled")
def test_the_probe_and_the_stub_compile(tmp_path):
    r = subprocess.run([CC, "-O1", "-march=i686", "-Wall", "-Werror", "-Wno-unused-function",
                        "-Wno-cast-function-type", "-o", str(tmp_path / "p.exe"),
                        str(CR / "tools" / "glide2probe.c"), "-lgdi32", "-luser32"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    r = subprocess.run([CC, "-shared", "-O1", "-Wall", "-Werror", "-o", str(tmp_path / "g.dll"),
                        str(STUB), "-Wl,--add-stdcall-alias"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


@pytest.mark.skipif(os.environ.get("GLIDE2PROBE_WINE") != "1",
                    reason="the Wine run of the harness takes minutes - GLIDE2PROBE_WINE=1")
def test_the_harness_under_wine():
    r = subprocess.run(["bash", str(CR / "tools" / "glide2probe_wine.sh")],
                       capture_output=True, text=True, timeout=1500)
    assert r.returncode == 0, r.stdout[-3000:] + r.stderr[-2000:]
