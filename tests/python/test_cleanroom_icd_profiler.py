"""voodoo-cleanroom ICD 0.1.67: the opt-in render-thread sampler (fxprof.c).

The V5 6000 is CPU-bound with four chips (Quake II ~215 fps at 640x480 and at
320x240 alike), and XP ships no profiler, so the ICD samples its own render
thread when RETROGL_PROF names an output file. Two properties matter enough to
pin:

* it is INERT unless asked for - a benchmark row must never carry a sampler;
* while the render thread is suspended the sampler calls only kernel entry
  points and a static table. A malloc, a printf or a loader call there can
  wait on a lock the suspended thread holds, and the game deadlocks.

The host half, scripts/benchmarks/icdprof.py, names the functions.
"""
import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "voodoo-cleanroom" / "patches" / "mesafx-voodoo2-icd.patch"


def _hunk(name):
    text = PATCH.read_text(errors="replace")
    m = re.search(rf"^diff --git a/{re.escape(name)} .*?(?=^diff --git |\Z)", text, re.S | re.M)
    assert m, f"{name} is not in the ICD patch"
    return m.group(0)


def _added(hunk):
    return "\n".join(l[1:] for l in hunk.splitlines() if l.startswith("+") and not l.startswith("+++"))


def test_sampler_is_inert_unless_retrogl_prof_is_set():
    src = _added(_hunk("src/mesa/drivers/glide/fxprof.c"))
    start = src.split("fxProfStart(void)", 1)[1].split("\n}", 1)[0]
    assert 'getenv("RETROGL_PROF")' in start
    assert start.index('getenv("RETROGL_PROF")') < start.index("CreateThread")


def test_sampler_loop_touches_nothing_that_takes_a_user_mode_lock():
    src = _added(_hunk("src/mesa/drivers/glide/fxprof.c"))
    loop = src.split("prof_main(LPVOID unused)", 1)[1].split("\n}", 1)[0]
    add = src.split("prof_add(DWORD eip)", 1)[1].split("\n}", 1)[0]
    for body in (loop, add):
        for banned in ("malloc", "printf", "fopen", "rgl_log", "LoadLibrary", "GetModuleFileName",
                       "VirtualQuery", "new ", "realloc"):
            assert banned not in body, f"{banned} inside the sampling path"
    assert "SuspendThread" in loop and "ResumeThread" in loop and "GetThreadContext" in loop


def test_hooks_are_where_the_render_thread_and_exit_are():
    wgl = _added(_hunk("src/mesa/drivers/glide/fxwgl.c"))
    api = _added(_hunk("src/mesa/drivers/glide/fxapi.c"))
    assert "fxProfStart();" in wgl and "fxProfFrame();" in wgl
    assert "fxProfStop();" in api
    mk = _hunk("src/mesa/Makefile.mgw")
    assert "+\tdrivers/glide/fxprof.c \\" in mk


def _icdprof():
    sys.path.insert(0, str(ROOT / "scripts" / "benchmarks"))
    spec = importlib.util.spec_from_file_location("icdprof", ROOT / "scripts" / "benchmarks" / "icdprof.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def test_icdprof_names_functions_in_our_modules_and_totals_the_rest():
    ip = _icdprof()
    text = ("# retrogl fxprof v1 samples=10 idle=5 dropped=0\n"
            "6fc825f8 4 6fc80000 C:\\Games\\Quake2Complete\\glide3x.dll\n"
            "6fcb09a4 3 6fc80000 C:\\Games\\Quake2Complete\\glide3x.dll\n"
            "0040120a 3 00400000 C:\\Games\\Quake2Complete\\quake2.exe\n")
    hdr, rows = ip.parse_profile(text)
    assert hdr == {"samples": 10, "idle": 5, "dropped": 0}
    tables = {"glide3x.dll": [(0x25f0, "_grGlideInit@0"), (0x309a0, "_hwcIdleHardwareWithTimeout")]}
    total, per_mod, per_fn = ip.summarize(rows, tables)
    assert total == 10
    assert per_mod["glide3x.dll"] == 7 and per_mod["quake2.exe"] == 3
    assert per_fn["glide3x.dll!_grGlideInit@0"] == 4
    assert per_fn["glide3x.dll!_hwcIdleHardwareWithTimeout"] == 3
    assert per_fn["quake2.exe!?"] == 3
    assert "70.0 %" in ip.report(hdr, rows, tables)
